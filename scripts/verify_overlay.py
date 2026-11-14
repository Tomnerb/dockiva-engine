#!/usr/bin/env python3
"""Image layers (--root UPPER --lower LOWER, native/overlay.h): overlay semantics
(tests/native/overlay_guest.c on every built runtime, each on a fresh upper layer
over one shared lower tree, the second run proving the first left the shared files
alone); new files kept in memory (--upper-memory, native/upm.h) renamed while open
over lower and upper files (tests/native/upm_guest.c), and what that run wrote
back: each file whole, and no whiteout beside it; hard links inside a bind mount
(tests/native/mountlink_guest.c), never across its boundary; then, with alpine as the lower
layer, busybox copying, archiving, renaming
and removing image files, and a container trying to change the image's files every
way it can (writes, chmod, rename, links, truncate, MAP_SHARED writes, sockets,
FIFOs; MAP_SHARED writes are overlay_guest's) while another container uses them. The lower tree must stay byte-for-byte,
mode, time and inode unchanged, and no copy-up leftovers may remain. Last, names that
differ only by case (tests/native/case_guest.c) with both layers on a case-sensitive
volume, as dockivad keeps them, with and without the memory layer, and statfs there
(tests/native/statfs_guest.c): the size of the volume --statfs-from names. And root's
CAP_DAC_OVERRIDE (tests/native/rootdac_guest.c): root changing 0555 and 000 directories
of the upper layer, a bind mount and the image, with and without the memory layer,
other users refused, the modes and the image left as they were, and a tar archive of
0555 directories and 0444 files extracted by busybox tar as root."""
import os
from pathlib import Path
import subprocess
import tempfile

from verify_procfs import alpine

ROOT = Path(__file__).resolve().parents[1]
T = 1000000000


def build(source, out):
    linker = next((Path.home()/'.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
    obj = out.with_suffix('.o')
    subprocess.run(['clang', '--target=aarch64-linux-gnu', '-O0', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-pic',
                    '-mgeneral-regs-only', '-c', str(source), '-o', str(obj)], check=True)
    subprocess.run([str(linker), '-m', 'aarch64elf', '-static', '-e', '_start', str(obj), '-o', str(out)], check=True)


def lower_tree(lower):
    """The tree overlay_guest.c expects (files 0644, directories 0755, times T)."""
    files = {'f': 'lower\n', 'suid': 's', 'd/a': 'a', 'd/b': 'b', 'd/sub/c': 'c', 'gone/x': 'x', 'h1': 'hard', 'map': 'L'*8192,
             'ro': 'ro', 'mv': 'mv', 'tdir/t': 't', 'open': 'open', 't1': 'trunc me', 'ch': 'chmod', 'ut': 'utimes', 'deep/1/2/3/f': 'deep',
             'x/y': 'y'}
    for d in ('e', 'edir2', 'ddir', 'cwd', 'ino/sub'):
        (lower/d).mkdir(parents=True)
    for name, text in files.items():
        (lower/name).parent.mkdir(parents=True, exist_ok=True)
        (lower/name).write_text(text)
        (lower/name).chmod(0o644)
    os.link(lower/'h1', lower/'h2')
    (lower/'suid').chmod(0o4755)
    os.symlink('f', lower/'ln')
    os.symlink('/d', lower/'abs')
    for base, dirs, names in os.walk(lower):
        for name in dirs+names:
            os.utime(os.path.join(base, name), (T, T), follow_symlinks=False)


def snapshot(path):
    out = {}
    for base, dirs, files in os.walk(path):
        for name in dirs+files:
            p = os.path.join(base, name)
            st = os.lstat(p)
            data = open(p, 'rb').read() if os.path.isfile(p) and not os.path.islink(p) else os.readlink(p) if os.path.islink(p) else None
            out[os.path.relpath(p, path)] = (st.st_mode, st.st_mtime_ns, st.st_size, st.st_ino, st.st_nlink, st.st_flags, data)
    return out


def leftovers(upper):
    return [str(p) for p in Path(upper).rglob('.wh..wh.cp*')]


def runtimes():
    for binary in ('dockiva-runtime', 'dockiva-hvf', 'dockiva-runtime-sanitize', 'dockiva-hvf-sanitize'):
        if (ROOT/'build'/binary).exists():
            yield binary, ROOT/'build'/binary, ['--cpu-backend', 'hvf', '--cpus', '4'] if 'hvf' in binary else []
        else:
            print(f'{binary}: skipped (not built)')


def mount_link_runs(t):
    """mountlink_guest.c on every built runtime, with and without the memory layer, bind
    mounts at /vol and /vol2; then what it left: one file under its three names in the
    first mount, nothing in the second, no whiteout in either."""
    failed = 0
    lower = t/'link-lower'
    (lower/'low').mkdir(parents=True)
    (lower/'low/f').write_text('f')
    build(ROOT/'tests/native/mountlink_guest.c', t/'mountlink')
    before = snapshot(lower)
    for binary, runtime, extra in runtimes():
        for memory in (False, True):
            tag = f'{binary}-{int(memory)}'
            upper, state, vol, vol2 = t/f'link-up-{tag}', t/f'link-state-{tag}', t/f'link-vol-{tag}', t/f'link-vol2-{tag}'
            for d in (upper, state, vol, vol2):
                d.mkdir()
            more = ['--upper-memory', str(state/'upper-sync'), '--layers-gen', str(state/'gen')] if memory else []
            r = subprocess.run([str(runtime), 'run', str(t/'mountlink'), '--root', str(upper), '--lower', str(lower), '--writable', '--bind', str(vol), '/vol', '--bind', str(vol2), '/vol2',
                                '--storage-limit-mib', '0', '--max-steps', '2000000000', '--process-tree', '--scheduled-network', '--dynamic', *more, *extra],
                               capture_output=True, timeout=300)
            ok = r.returncode == 0 and r.stdout == b'mountlink PASS\n' and b'ERROR: AddressSanitizer' not in r.stderr
            names = sorted(str(p.relative_to(vol)) for p in vol.rglob('*'))
            ok = ok and names == ['a', 'b', 'd', 'd/c', 's'] and len({(vol/n).stat().st_ino for n in ('a', 'b', 'd/c')}) == 1 and (vol/'a').stat().st_nlink == 3
            ok = ok and not os.listdir(vol2) and not os.path.lexists(upper/'x') and (upper/'low/g').is_file()
            failed += not ok
            print(f'{binary} mountlink_guest{" with the memory layer" if memory else ""}:', 'PASS' if ok else
                  f'FAIL exit={r.returncode} {r.stdout!r} vol={names} vol2={os.listdir(vol2)} {r.stderr.decode()[-1500:]}')
    same = snapshot(lower) == before
    failed += not same
    print('mountlink lower layer unchanged:', 'PASS' if same else 'FAIL')
    return failed


# upm_guest.c's lower tree, and what its run leaves in the upper layer: files written
# back, and the whiteouts of the lower files it removed (and only those).
UPM_LOWER = {'u/lower': 'old lower', 'u/lower2': 'old lower2', 'u/up': 'old up', 'u/app.log.1': 'old log', 'u/k': 'k', 'u/nr': 'nr',
             'u/closed': 'old closed', 'u/sub/s': 's', 'w/x': 'old w', 'v/keep': 'v'}
UPM_WRITTEN = {'u/lower': 'newmore', 'u/up': 'y', 'u/first': 'z', 'u/app.log.1': 'ab', 'u/app.log': 'c', 'w/x': 'w', 'v/made': 'v',
               'u/m2': 'm', 'u/closed': 'c', 'u/r.new': ''}
UPM_WHITEOUTS = {'u/.wh.k', 'u/.wh.lower2'}


def upm_runs(t):
    """upm_guest.c on every built runtime, as dockivad runs a container (--upper-memory, --layers-gen)."""
    failed = 0
    lower = t/'upm-lower'
    for name, text in UPM_LOWER.items():
        (lower/name).parent.mkdir(parents=True, exist_ok=True)
        (lower/name).write_text(text)
    build(ROOT/'tests/native/upm_guest.c', t/'upm')
    before = snapshot(lower)
    for binary, runtime, extra in runtimes():
        upper, state = t/f'upm-up-{binary}', t/f'upm-state-{binary}'
        upper.mkdir()
        state.mkdir()
        r = subprocess.run([str(runtime), 'run', str(t/'upm'), '--root', str(upper), '--lower', str(lower), '--writable',
                            '--upper-memory', str(state/'upper-sync'), '--layers-gen', str(state/'gen'), '--storage-limit-mib', '0',
                            '--max-steps', '2000000000', '--process-tree', '--scheduled-network', '--dynamic', *extra], capture_output=True, timeout=300)
        wrong = {k: (upper/k).read_text() if (upper/k).is_file() else None for k, v in UPM_WRITTEN.items()
                 if not (upper/k).is_file() or (upper/k).read_text() != v}
        hidden = {str(p.relative_to(upper)) for p in upper.rglob('.wh.*')}
        left = [k for k in ('u/k', 'u/lower2', 'u/nr', 'u/x.new', 'top.new', 'top2.new') if (upper/k).exists()]
        ok = r.returncode == 0 and r.stdout == b'upm PASS\n' and b'ERROR: AddressSanitizer' not in r.stderr
        ok = ok and not wrong and hidden == UPM_WHITEOUTS and not left
        failed += not ok
        print(f'{binary} upm_guest (--upper-memory):', 'PASS' if ok else f'FAIL exit={r.returncode} {r.stdout!r} written={wrong} '
              f'whiteouts={sorted(hidden)} left={left} {r.stderr.decode()[-2000:]}')
    same = snapshot(lower) == before
    failed += not same
    print('upm lower layer unchanged:', 'PASS' if same else 'FAIL')
    return failed


def dac_runs(t):
    """rootdac_guest.c on every built runtime, with and without the memory layer, a
    bind mount at /vol; then what it left: every file on disk, each mode as the guest set it."""
    failed = 0
    lower = t/'dac-lower'
    for name in ('low/ro/x', 'low/ro/mv', 'low/ro/keep', 'low/ro2/x'):
        (lower/name).parent.mkdir(parents=True, exist_ok=True)
        (lower/name).write_text(name.rsplit('/', 1)[1])
    (lower/'low/ro/sub').mkdir()
    (lower/'low/ro').chmod(0o555)
    (lower/'low/ro2').chmod(0o555)
    build(ROOT/'tests/native/rootdac_guest.c', t/'rootdac')
    before = snapshot(lower)

    def left(base, hard=True):
        """What is wrong under base (the host's /w or /vol) after the run (hard: it made a hard link there)."""
        a = base/'a'
        wrong = [] if a.stat().st_mode & 0o7777 == 0o555 else [f'a is {oct(a.stat().st_mode)}']
        a.chmod(0o755)                                    # to look inside
        want = {'d1b': 'd', 'd3': 'd+', 'd4': 'd', 'd5': 'd', 'y1b': 'y', 'y3': 'y', 'keep': 'kept', 'z': 'z', 'cwd': 'c'}
        if (a/'d3').stat().st_mode & 0o7777 != 0:
            wrong.append('d3 mode')
        (a/'d3').chmod(0o600)
        wrong += [k for k, v in want.items() if not (a/k).is_file() or (a/k).read_text() != v]
        wrong += [k for k in ('d/in', 'pre2', 'last') if not (a/k).is_dir()] + [k for k in ('pre', 'd1', 'd2', 'y1', 'y2', 'f', 'g', 'mvd') if os.path.lexists(a/k)]
        if not (a/'l').is_symlink() or not (a/'p').is_fifo() or (base/'out').stat().st_mode & 0o7777 != 0o555:
            wrong.append('link, fifo or moved directory')
        if hard and (not (a/'h').is_file() or (a/'h').stat().st_ino != (a/'d4').stat().st_ino):
            wrong.append('hard link')
        return wrong + [str(p) for p in base.rglob('.wh.*')]

    for binary, runtime, extra in runtimes():
        for memory in (False, True):
            tag = f'{binary}-{int(memory)}'
            upper, state, vol, ro = t/f'dac-up-{tag}', t/f'dac-state-{tag}', t/f'dac-vol-{tag}', t/f'dac-ro-{tag}'
            for d in (upper, state, vol, ro/'locked'):
                d.mkdir(parents=True)
            (ro/'locked/f').write_text('f')
            (ro/'locked').chmod(0)
            more = ['--upper-memory', str(state/'upper-sync'), '--layers-gen', str(state/'gen')] if memory else []
            r = subprocess.run([str(runtime), 'run', str(t/'rootdac'), '--root', str(upper), '--lower', str(lower), '--writable', '--bind', str(vol), '/vol', '--bind-ro', str(ro), '/ro',
                                '--storage-limit-mib', '0', '--max-steps', '2000000000', '--process-tree', '--scheduled-network', '--dynamic', *more, *extra],
                               capture_output=True, timeout=300)
            ok = r.returncode == 0 and r.stdout == b'rootdac PASS\n' and b'ERROR: AddressSanitizer' not in r.stderr
            wrong = []
            if ok:
                low = upper/'low/ro'
                wrong = left(upper/'w') + left(vol, False) + ([] if low.stat().st_mode & 0o7777 == 0o555 else ['low/ro mode'])
                low.chmod(0o755)
                wrong += [k for k in ('.wh.x', '.wh.sub', '.wh.mv', 'f', 'mv2') if not (low/k).is_file()] + ([] if (low/'new').is_dir() else ['low/ro/new'])
                wrong += [] if (ro/'locked').stat().st_mode & 0o7777 == 0 else ['the read-only mount changed']
                wrong += ['low/ro2 copied up'] if (upper/'low/ro2').exists() and (upper/'low/ro2').stat().st_mode & 0o7777 != 0o555 else []
            ok = ok and not wrong
            failed += not ok
            print(f'{binary} rootdac_guest{" with the memory layer" if memory else ""}:', 'PASS' if ok else
                  f'FAIL exit={r.returncode} {r.stdout!r} wrong={wrong} {r.stderr.decode()[-1500:]}')
    same = snapshot(lower) == before
    failed += not same
    print('rootdac lower layer unchanged:', 'PASS' if same else 'FAIL')
    return failed


ATTACK = r'''
for f in /bin/busybox /lib/ld-musl-aarch64.so.1; do      # what this shell runs on: changed, not broken
  echo pwn >> $f; chmod 777 $f; touch -d 2001-01-01 $f; ln $f $f.hard; mv $f.hard $f.moved; rm $f.moved
done
for f in /etc/motd /etc/hostname /etc/fstab /etc/apk/world; do
  echo pwn >> $f; echo pwn > $f.new; chmod 777 $f; touch -d 2001-01-01 $f; truncate -s 1 $f; ln $f $f.hard; echo pwn >> $f.hard
  mv $f $f.moved; mv $f.moved $f; ln -s /x $f.sl; mkfifo $f.fifo; rm -f $f.new $f.hard $f.sl $f.fifo
done
rm -rf /usr/share /etc/apk; mkdir /etc/apk; echo pwn > /etc/apk/world; mv /lib/apk /lib/apk2; cp -a /sbin /sbin2; rm -rf /sbin; mv /sbin2 /sbin
nc -lU /etc/sock & sleep 1; kill $! 2>/dev/null
cat /etc/motd /etc/apk/world; echo ==attack done=='''


def main():
    failed = 0
    alpine_root = alpine()
    with tempfile.TemporaryDirectory(prefix='ovl') as temp:
        t = Path(temp)
        lower = t/'lower'
        lower.mkdir()
        lower_tree(lower)
        build(ROOT/'tests/native/overlay_guest.c', t/'overlay')
        failed += mount_link_runs(t)
        before = snapshot(lower)
        for binary, runtime, extra in runtimes():
            for run in (1, 2):                              # the second run sees the lower layer as the first did, with the lookup cache
                upper = t/f'up-{binary}-{run}'
                upper.mkdir()
                cache = ['--layers-gen', str(t/f'gen-{binary}')] if run == 2 else []
                r = subprocess.run([str(runtime), 'run', str(t/'overlay'), '--root', str(upper), '--lower', str(lower), '--writable',
                                    '--storage-limit-mib', '0', '--max-steps', '2000000000', '--process-tree', '--scheduled-network', '--dynamic', *extra, *cache], capture_output=True, timeout=300)
                ok = r.returncode == 0 and r.stdout == b'overlay PASS\n' and b'ERROR: AddressSanitizer' not in r.stderr
                markers = all((upper/p).exists() for p in ('d/.wh.a', 'gone/.wh..wh..opq', '.wh.mv', '.wh.open', 'edir2/.wh..wh..opq'))
                ok = ok and markers and not leftovers(upper) and not (upper/'.wh.gone').exists()
                failed += not ok
                print(f'{binary} overlay_guest run {run}:', 'PASS' if ok else f'FAIL exit={r.returncode} {r.stdout!r} markers={markers} '
                      f'leftovers={leftovers(upper)} {r.stderr.decode()[-2000:]}')
        after = snapshot(lower)
        same = after == before
        failed += not same
        print('lower layer unchanged:', 'PASS' if same else f'FAIL {[k for k in set(before)|set(after) if before.get(k) != after.get(k)]}')
        failed += upm_runs(t)
        failed += dac_runs(t)

        # busybox on alpine's files as the lower layer
        lower = t/'alpine'
        subprocess.run(['cp', '-c', '-R', str(alpine_root), str(lower)], check=True)
        before = snapshot(lower)

        def busybox(upper, script, *more, background=False):
            upper.mkdir(exist_ok=True)
            command = [str(ROOT/'build/dockiva-hvf'), 'run', str(lower/'bin/busybox'), '--root', str(upper), '--lower', str(lower), '--writable',
                       '--storage-limit-mib', '0', '--dynamic', '--cpu-backend', 'hvf', '--process-tree', '--scheduled-network', '--signals',
                       '--cpus', '4', '--memory-limit-mib', '1024', '--elf-limit-mib', '64', '--mmap-limit-mib', '4096',
                       '--max-steps', '18446744073709551615', '--env', 'PATH=/usr/sbin:/usr/bin:/sbin:/bin', *more, '--argv0', 'sh', '--', '-c', script]
            if background:
                return subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            return subprocess.run(command, capture_output=True, timeout=600)

        reader = busybox(t/'reader', 'i=0; while [ $i -lt 40 ]; do cat /etc/motd /bin/busybox >/dev/null; ls /usr/share >/dev/null; i=$((i+1)); done; '
                         'cat /etc/motd; ls /etc/apk/world /usr/share/apk >/dev/null && echo ==reader done==', background=True)
        script = ('set -e; cp -a /usr/lib /tmp/e; diff -r /usr/lib /tmp/e; tar cf /tmp/t.tar /usr /lib 2>/dev/null; mkdir /opt/x; tar xf /tmp/t.tar -C /opt/x; '
                  'diff -r /usr /opt/x/usr; mv /opt/x /opt/y; rm -r /tmp/e /tmp/t.tar /opt/y; echo extra >> /etc/motd; rm /etc/hostname; '
                  'mv /etc/profile /etc/profile2; rm -rf /usr/share/apk; mkdir /usr/share/apk; test -z "$(ls -A /usr/share/apk)"; '
                  'test ! -e /etc/hostname; test -e /etc/profile2; find / -xdev | grep -c . >/dev/null; grep -q extra /etc/motd; echo e2e PASS')
        r = busybox(t/'e2e', script)
        ok = r.returncode == 0 and r.stdout.endswith(b'e2e PASS\n') and not leftovers(t/'e2e')
        failed += not ok
        print('busybox e2e on alpine layers:', 'PASS' if ok else f'FAIL exit={r.returncode} {r.stdout[-800:]!r} {r.stderr.decode()[-1500:]}')
        # tar as root: 0555 directories made with their final mode, then filled; rm -rf after
        script = ('set -e; mkdir -p /src/m/a/b /src/m/c; echo 1 > /src/m/a/f; echo 2 > /src/m/a/b/g; echo 3 > /src/m/c/h; chmod 444 /src/m/a/f /src/m/a/b/g /src/m/c/h; '
                  'chmod 555 /src/m/a/b /src/m/a /src/m/c /src/m; tar cf /t.tar -C /src m; mkdir /dst; tar xf /t.tar -C /dst; test "$(find /dst | wc -l)" = 8; '
                  'test "$(stat -c %a /dst/m/a)" = 555; test "$(stat -c %a /dst/m/a/b/g)" = 444; test "$(cat /dst/m/a/b/g)" = 2; rm -rf /dst /src; test ! -e /dst; test ! -e /src; echo tar PASS')
        r = busybox(t/'tar', script, '--upper-memory', str(t/'tar-sync'))
        ok = r.returncode == 0 and r.stdout.endswith(b'tar PASS\n') and not leftovers(t/'tar')
        failed += not ok
        print('busybox tar and rm -rf of 0555 directories as root:', 'PASS' if ok else f'FAIL exit={r.returncode} {r.stdout[-800:]!r} {r.stderr.decode()[-1500:]}')
        r = busybox(t/'attack', ATTACK)
        out = r.stdout.decode(errors='replace')
        ok = '==attack done==' in out and 'pwn' in out    # the container sees its own changes
        failed += not ok
        print('attack container ran:', 'PASS' if ok else f'FAIL exit={r.returncode} {out[-800:]} {r.stderr.decode()[-1500:]}')
        rout, rerr = reader.communicate(timeout=600)
        ok = reader.returncode == 0 and b'==reader done==' in rout and b'pwn' not in rout
        failed += not ok
        print('a second container meanwhile sees the image unchanged:', 'PASS' if ok else f'FAIL {rout[-500:]!r} {rerr.decode()[-800:]}')
        r = busybox(t/'third', 'cat /etc/motd /etc/hostname >/dev/null && ls /usr/share/apk /lib/apk >/dev/null && ! grep -q pwn /etc/motd /etc/fstab /etc/apk/world && echo clean')
        ok = r.stdout == b'clean\n'
        failed += not ok
        print('a new container afterwards sees the image unchanged:', 'PASS' if ok else f'FAIL {r.stdout!r} {r.stderr.decode()[-800:]}')
        after = snapshot(lower)
        same = after == before
        failed += not same
        print('alpine lower layer unchanged:', 'PASS' if same else f'FAIL {sorted(k for k in set(before)|set(after) if before.get(k) != after.get(k))[:20]}')
    failed += case_sensitive()
    return 1 if failed else 0


CASE_LOWER = {'Lower': 'L', 'inc/xt_CONNMARK.h': 'MARK', 'inc/xt_connmark.h': 'mark', 'Dir/x': 'x'}
CASE_UPPER = {'FOO': 'F', 'foo': 'f', 'D/in': '1', 'lower': 'u', 'Bar': 'B', 'bar': 'b'}   # what case_guest.c leaves


def case_sensitive():
    """case_guest.c on every built runtime, its layers on a case-sensitive APFS sparse
    bundle attached as dockivad attaches its store; the upper directory must then hold
    both names of each pair, the memory layer's written back."""
    failed = 0
    with tempfile.TemporaryDirectory(prefix='ovlcase') as temp:
        t = Path(temp)
        build(ROOT/'tests/native/case_guest.c', t/'case')
        subprocess.run(['hdiutil', 'create', '-quiet', '-type', 'SPARSEBUNDLE', '-fs', 'Case-sensitive APFS', '-layout', 'NONE', '-size', '1g', str(t/'cs.sparsebundle')], check=True)
        (t/'cs').mkdir()
        subprocess.run(['diskutil', 'image', 'attach', '-nobrowse', '-mountPoint', str(t/'cs'), str(t/'cs.sparsebundle')], check=True, capture_output=True)
        try:
            lower = t/'cs/lower'
            for name, text in CASE_LOWER.items():
                (lower/name).parent.mkdir(parents=True, exist_ok=True)
                (lower/name).write_text(text)
            before = snapshot(lower)
            for binary, runtime, extra in runtimes():
                for memory in ([], ['--upper-memory', str(t/'sync')]):
                    upper = t/'cs'/f'up-{binary}-{len(memory)}'
                    upper.mkdir()
                    r = subprocess.run([str(runtime), 'run', str(t/'case'), '--root', str(upper), '--lower', str(lower), '--writable', '--storage-limit-mib', '0',
                                        '--max-steps', '2000000000', '--process-tree', '--scheduled-network', '--dynamic', *memory, *extra], capture_output=True, timeout=300)
                    files = all((upper/n).is_file() and (upper/n).read_text() == v for n, v in CASE_UPPER.items())
                    links = (upper/'L.new').is_symlink() and os.readlink(upper/'L.new') == 'x' and not os.path.lexists(upper/'l.new')
                    ok = r.returncode == 0 and r.stdout == b'case PASS\n' and b'ERROR: AddressSanitizer' not in r.stderr and files and links and \
                        (upper/'d').is_dir() and (upper/'dir').is_dir() and (upper/'.wh.Lower').exists() and not os.path.lexists(upper/'Foo')
                    failed += not ok
                    print(f'{binary} case_guest{" with the memory layer" if memory else ""}:', 'PASS' if ok else
                          f'FAIL exit={r.returncode} {r.stdout!r} upper={sorted(os.listdir(upper))} {r.stderr.decode()[-1500:]}')
            # statfs_guest.c: the size of the file system --statfs-from names (dockivad: the
            # engine home, on the Mac's volume), else the image's 1 GB under the root.
            build(ROOT/'tests/native/statfs_guest.c', t/'statfs')
            for binary, runtime, extra in runtimes():
                for memory in ([], ['--upper-memory', str(t/'sync')]):
                    for space in (t, None):
                        upper = t/'cs'/f'statfs-{binary}-{len(memory)}-{space is None}'
                        upper.mkdir()
                        r = subprocess.run([str(runtime), 'run', str(t/'statfs'), '--root', str(upper), '--lower', str(lower), '--writable', '--storage-limit-mib', '0', '--max-steps', '2000000000',
                                            '--process-tree', '--scheduled-network', '--dynamic', *memory, *(['--statfs-from', str(space)] if space else []), *extra],
                                           capture_output=True, timeout=300)
                        want = os.statvfs(space or t/'cs')
                        want = want.f_blocks * want.f_frsize
                        got = [int.from_bytes(r.stdout[i+16:i+24], 'little') * int.from_bytes(r.stdout[i+8:i+16], 'little') for i in (0, 120, 240)]
                        ok = r.returncode == 0 and len(r.stdout) == 360 and b'ERROR: AddressSanitizer' not in r.stderr and all(abs(g - want) <= want // 100 for g in got)
                        failed += not ok
                        print(f'{binary} statfs{" with the memory layer" if memory else ""}, {"--statfs-from" if space else "no --statfs-from"}:',
                              'PASS' if ok else f'FAIL exit={r.returncode} sizes={got} want={want} {r.stderr.decode()[-800:]}')
            same = snapshot(lower) == before
            failed += not same
            print('case lower layer unchanged:', 'PASS' if same else 'FAIL')
        finally:
            subprocess.run(['hdiutil', 'detach', '-force', str(t/'cs')], capture_output=True)
    return failed


if __name__ == '__main__':
    raise SystemExit(main())
