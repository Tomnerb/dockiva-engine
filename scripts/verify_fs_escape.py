#!/usr/bin/env python3
"""Guest paths never leave the container root or write through a read-only mount
(alpine busybox under the hvf runtime): symlinks (relative, absolute, "..", inside
bind mounts) and ".." at the root or a mount's top, for every path call busybox makes
(open, stat, mkdir, unlink, rename, link, symlink, mknod, chmod, utimensat, truncate,
chdir), and while another process swaps a directory for an escaping symlink (busybox,
and tests/native/fs_escape_guest.c, whose calls race the swap far more often).
Afterwards the host files beside the root and the read-only mount are unchanged. Run
twice: on a root of its own, and as an empty upper layer over the image's files
(--lower, native/overlay.h), which must stay unchanged as well."""
import os
from pathlib import Path
import subprocess
import tempfile

from verify_procfs import alpine

ROOT = Path(__file__).resolve().parents[1]
SECRET = 'HOST-SECRET-7f3a'

ATTEMPTS = r'''
try() { for p in "$@"; do
  cat $p/secret; echo pwn > $p/pwned; echo pwn >> $p/secret; mkdir $p/pd; touch $p/secret; chmod 777 $p/secret
  truncate -s 0 $p/secret; mv $p/secret $p/moved; mv /tmp/mine $p/mine; ln $p/secret /tmp/hard; ln -s x $p/sl; mkfifo $p/fifo
  rm -f $p/secret; rm -rf $p/sub; ls -la $p/; cat $p/sub/inner; (cd $p && cat secret && cd .. && ls)
done; }
'''

SCRIPT = ATTEMPTS + r'''
echo mine > /tmp/mine
try /esc_rel /esc_abs /deep/l /deep/l/ /vol/up /vol/abs /vol/../../outside /../outside /race/../../outside ../../outside 2>&1
cd /vol && try ../outside up 2>&1; cd /
echo ==ro==
for p in /ro /rolink /vol/ro_via_vol; do
  echo x > $p/new; echo x >> $p/keep; rm -f $p/keep; mkdir $p/d; touch $p/keep; chmod 777 $p/keep; mv $p/keep $p/k2
  mv /tmp/mine $p/mine; ln -s a $p/s; mkfifo $p/f; truncate -s 0 $p/keep; ln /etc/hostname $p/h; rm -rf $p/sub
done 2>&1
echo ==race==
mkdir -p /race
( i=0; while [ $i -lt 300 ]; do rm -rf /race/d; mkdir /race/d; rm -rf /race/d; ln -s ../../outside /race/d; i=$((i+1)); done ) &
( i=0; while [ $i -lt 300 ]; do rm -rf /race/s/d; mkdir -p /race/s/d; rm -rf /race/s; ln -s ../../outside/.. /race/s; i=$((i+1)); done ) &
j=0;hit=0
while [ $j -lt 300 ]; do
  echo pwn > /race/d/pwned && hit=$((hit+1)); mkdir /race/d/pd; touch /race/d/secret; chmod 777 /race/d/secret; truncate -s 0 /race/d/secret
  cat /race/d/secret; rm -f /race/d/secret; ln -s x /race/d/sl; mv /race/d/secret /race/d/s2; echo pwn > /race/s/outside/pwned2
  j=$((j+1))
done 2>/dev/null
wait
echo ==done== $hit
'''


def snapshot(path):
    out = {}
    for base, dirs, files in os.walk(path):
        for name in dirs+files:
            p = os.path.join(base, name)
            st = os.lstat(p)
            data = open(p, 'rb').read() if os.path.isfile(p) and not os.path.islink(p) else os.readlink(p) if os.path.islink(p) else None
            out[os.path.relpath(p, path)] = (st.st_mode, st.st_mtime_ns, st.st_size, data)
    return out


def guest_program(folder):
    linker = next((Path.home()/'.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
    obj, elf = folder/'fs_escape.o', folder/'fs_escape.elf'
    subprocess.run(['clang', '--target=aarch64-linux-gnu', '-O1', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-pic',
                    '-c', str(ROOT/'tests/native/fs_escape_guest.c'), '-o', str(obj)], check=True)
    subprocess.run([str(linker), '-m', 'aarch64elf', '-static', '-e', '_start', str(obj), '-o', str(elf)], check=True)
    return elf


def main():
    alpine_root = alpine()
    for layered in (False, True):
        with tempfile.TemporaryDirectory(prefix='fsesc') as temp:
            print(escape(Path(temp), alpine_root, layered))


def escape(t, alpine_root, layered):
    """layered: the escaping symlinks and busybox are image files (--lower), the root an
    empty upper layer; the image's files must stay unchanged too."""
    root = t/'root'
    image = t/'image' if layered else root
    subprocess.run(['cp', '-c', '-R', str(alpine_root), str(image)], check=True)
    if layered:
        root.mkdir()
    for d in ('outside/sub', 'vol', 'ro/sub'):
        (t/d).mkdir(parents=True)
    (t/'outside/secret').write_text(SECRET)
    (t/'outside/secret').chmod(0o600)
    (t/'outside/sub/inner').write_text(SECRET)
    (t/'ro/keep').write_text('keep')
    (t/'ro/sub/inner').write_text('inner')
    os.symlink('../outside', t/'vol/up')                  # host-relative: the host's parent of the mount
    os.symlink(str(t/'outside'), t/'vol/abs')              # a host path is a guest path
    os.symlink('/ro', t/'vol/ro_via_vol')
    os.symlink('../outside', image/'esc_rel')
    os.symlink(str(t/'outside'), image/'esc_abs')
    (image/'deep').mkdir()
    os.symlink('../../outside', image/'deep/l')
    os.symlink('/ro', image/'rolink')
    (image/'race').mkdir()
    subprocess.run(['cp', str(guest_program(t)), str(image/'escape')], check=True)
    layer = ['--lower', str(image)] if layered else []
    before = {'outside': snapshot(t/'outside'), 'ro': snapshot(t/'ro'), 'top': sorted(os.listdir(t)), 'image': snapshot(image) if layered else None}
    command = [str(ROOT/'build/dockiva-hvf'), 'run', str(image/'bin/busybox'), '--root', str(root), *layer, '--writable', '--storage-limit-mib', '0', '--dynamic',
               '--cpu-backend', 'hvf', '--process-tree', '--scheduled-network', '--cpus', '4', '--memory-limit-mib', '512',
               '--elf-limit-mib', '64', '--mmap-limit-mib', '2048', '--bind', str(t/'vol'), '/vol', '--bind-ro', str(t/'ro'), '/ro',
               '--env', 'PATH=/usr/sbin:/usr/bin:/sbin:/bin', '--argv0', 'sh', '--', '-c', SCRIPT]
    r = subprocess.run(command, capture_output=True, timeout=600)
    out = r.stdout.decode(errors='replace')
    assert r.returncode == 0, (r.returncode, out[-2000:], r.stderr.decode()[-2000:])
    race = subprocess.run([str(ROOT/'build/dockiva-hvf'), 'run', str(image/'escape'), '--root', str(root), *layer, '--writable', '--storage-limit-mib', '0', '--dynamic',
                           '--cpu-backend', 'hvf', '--process-tree', '--cpus', '4'], capture_output=True, timeout=600)
    race_out = race.stdout.decode(errors='replace')
    assert race.returncode == 0 and race_out.startswith('race rounds'), (race.returncode, race_out, race.stderr.decode()[-2000:])
    assert '==done==' in out, out[-2000:]
    assert SECRET not in out, 'a host file was read: '+out
    after = {'outside': snapshot(t/'outside'), 'ro': snapshot(t/'ro'), 'top': sorted(os.listdir(t)), 'image': snapshot(image) if layered else None}
    assert after == before, ('host files changed', [k for k in before if before[k] != after[k]])
    ro = out.split('==ro==')[1].split('==race==')[0]
    assert 'Read-only file system' in ro, ro
    assert (t/'vol/up').is_symlink() and not (root/'outside').exists()
    assert (root/'tmp/mine').read_text() == 'mine\n'      # never moved into a read-only mount
    return (('fs escape ok (image layers)' if layered else 'fs escape ok')+'; busybox race writes that landed in the guest directory: '+
            out.split('==done==')[1].split()[0]+' of 300; '+race_out.strip())


if __name__ == '__main__':
    main()
