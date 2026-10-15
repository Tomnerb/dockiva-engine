#!/usr/bin/env python3
"""Run the fork, socket, epoll, identity and thread guests (including glibc-style fork) on every built CPU backend."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def build(name, folder, linker):
    obj, elf = folder/(name+'.o'), folder/(name+'.elf')
    source = ROOT/'tests/native'/(name+'.c')
    if not source.exists():  # assembly guests
        source = source.with_suffix('.S')
    # fp_preserve_guest uses SIMD registers in inline assembly.
    c_only = ['-mgeneral-regs-only'] if source.suffix == '.c' and name != 'fp_preserve_guest' else []
    subprocess.run(['clang', '--target=aarch64-linux-gnu', '-O0', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                    '-fno-pic', *c_only, '-c', str(source), '-o', str(obj)], check=True)
    subprocess.run([str(linker), '-m', 'aarch64elf', '-static', '-e', '_start', str(obj), '-o', str(elf)], check=True)
    return elf


def main():
    folder = ROOT/'build/fork-sockets'
    folder.mkdir(parents=True, exist_ok=True)
    linker = next((Path.home()/'.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
    server, guest, leaf, epoll, edge, forks, identity, threads, fork_exec, exec_leaf, fork_threads = (build(n, folder, linker) for n in (
        'fork_server_guest', 'exec_socket_guest', 'exec_socket_leaf', 'epoll_sched_guest', 'epoll_et_guest', 'fork_guest',
        'identity_guest', 'threads_guest', 'fork_exec_guest', 'fork_exec_leaf', 'fork_threads_guest'))
    thread_group, futex, thread_limit, fp_preserve, sleep, lazy, writev, many, vdso, udp, unix, session, fdtable, sigmask = (build(n, folder, linker) for n in ('thread_group_guest', 'futex_guest', 'thread_limit_guest', 'fp_preserve_guest', 'sleep_guest', 'lazy_memory_guest', 'writev_guest', 'many_sockets_guest', 'vdso_guest', 'udp_guest', 'unix_guest', 'session_guest', 'fd_table_guest', 'sigmask_guest'))
    # Parallel workers (--cpus): copy-on-write across concurrent forks, shared-memory atomics.
    cow_stress, smp_atomic, fork_churn, thread_smp, out_edge, around = (build(n, folder, linker) for n in ('cow_stress_guest', 'smp_atomic_guest', 'fork_churn_guest', 'thread_smp_guest', 'epoll_out_edge_guest', 'fault_around_guest'))
    setuid, setuid_leaf, ping, many_procs, inet6, bigfile = (build(n, folder, linker) for n in ('setuid_guest', 'setuid_leaf', 'ping_guest', 'many_procs_guest', 'inet6_guest', 'bigfile_guest'))
    setuid_root = folder/'setuid-root'  # set-ID leaves, run as uid/gid 1000
    shutil.rmtree(setuid_root, ignore_errors=True)
    setuid_root.mkdir()
    for name, mode in (('suid', 0o4755), ('sgid', 0o2755), ('plain', 0o755)):
        shutil.copy(setuid_leaf, setuid_root/name)
        os.chown(setuid_root/name, -1, os.getgid())  # macOS drops setgid for a group the user is not in
        (setuid_root/name).chmod(mode)
    exec_root = folder/'exec-root'  # fork_exec_guest execs /exec.elf after /bad.elf fails with ENOEXEC
    shutil.rmtree(exec_root, ignore_errors=True)
    exec_root.mkdir()
    shutil.copy2(exec_leaf, exec_root/'exec.elf')
    (exec_root/'bad.elf').write_bytes(b'bad ELF')
    (exec_root/'bad.elf').chmod(0o700)
    shared = folder/'shared-root'  # fork_guest creates /shared in a writable root
    root = folder/'root'
    shutil.rmtree(root, ignore_errors=True)
    root.mkdir()
    shutil.copy2(leaf, root/'leaf')
    common = ['--memory-limit-mib', '64', '--max-steps', '2000000000']
    failed = 0
    cpus = int(sys.argv[sys.argv.index('--cpus')+1]) if '--cpus' in sys.argv else 1  # hvf workers running tasks at once
    for backend, binary in (('interpreter', 'dockiva-runtime'), ('hvf', 'dockiva-hvf')):
        runtime = ROOT/'build'/binary
        if not runtime.exists():
            print(f'{backend}: skipped ({runtime} not built)')
            continue
        # Both backends block through the scheduler; hvf also maps whole pages (--dynamic).
        extra = ['--cpu-backend', 'hvf', '--dynamic', '--cpus', str(cpus)] if backend == 'hvf' else []
        extra += ['--process-tree', '--scheduled-network']
        for elf, args, expected in ((server, [], b'fork-server PASS\n'), (guest, ['--root', str(root)], b'exec-socket PASS\n'), (epoll, [], b'epoll-sched PASS\n'), (edge, [], b'epoll-et PASS\n'), (forks, ['--root', str(shared), '--writable'], b'fork PASS\n'), (identity, ['--root', str(shared), '--writable'], b'identity PASS\n'), (threads, [], b'threads PASS\n'),
                                     (fork_exec, ['--root', str(exec_root), '--writable'], b''), (fork_threads, [], b''),
                                     (thread_group, [], b''), (futex, [], struct.pack('<I', 11)), (thread_limit, [], b''),
                                     (fp_preserve, [], b'fp-preserve PASS\n'), (sleep, [], b'sleep PASS\n'), (lazy, ['--mmap-limit-mib', '2048'], b'lazy-memory PASS\n'), (writev, [], b'x'*64+b'\n'), (many, [], b'many-sockets PASS\n'), (vdso, [], b'vdso PASS\n'), (udp, [], b'udp loopback PASS\n'), (udp, ['--outbound'], b'udp outbound PASS\n'), (unix, ['--root', str(shared), '--writable'], b'unix PASS\n'), (session, [], b'session PASS\n'), (fdtable, ['--root', str(shared), '--writable'], b'fd-table PASS\n'), (sigmask, ['--signals'], b'sigmask PASS\n'), (setuid, ['--root', str(setuid_root), '--uid', '1000', '--gid', '1000'], b'setuid PASS\n'), (ping, [], b'ping PASS\n'), (many_procs, ['--memory-limit-mib', '512'], b'many-procs PASS\n'), (inet6, [], b'inet6 PASS\n'), (bigfile, ['--root', str(shared), '--writable', '--file-limit-mib', '1048576', '--storage-limit-mib', '0'], b'bigfile PASS\n'),
                                     (cow_stress, [], b'cow-stress PASS\n'), (smp_atomic, [], b'smp-atomic PASS\n'), (fork_churn, [], b'fork-churn PASS\n'), (thread_smp, [], b'thread-smp PASS\n'), (out_edge, [], b'epoll-out-edge PASS\n'),
                                     (around, ['--mmap-limit-mib', '256', '--signals'], b'fault-around PASS\n')):
            if backend == 'interpreter' and elf in (threads, fork_threads, thread_group, futex, thread_limit, fp_preserve, sleep, vdso, sigmask, cow_stress, smp_atomic, fork_churn, thread_smp, out_edge, around):
                continue  # the interpreter has no CLONE_THREAD support (nor these SIMD instructions)
            shutil.rmtree(shared, ignore_errors=True)
            shared.mkdir()
            r = subprocess.run([str(runtime), 'run', str(elf), *args, *common, *extra], capture_output=True, timeout=60)
            ok = r.returncode == 0 and r.stdout == expected
            failed += not ok
            print(f'{backend} {elf.stem}: {"PASS" if ok else "FAIL"} exit={r.returncode} {r.stdout.decode().strip()} {r.stderr.decode().strip()}')
        # A set-user-ID first program (docker exec -u 1000 of one) runs as root too: setuid_leaf exits 10.
        r = subprocess.run([str(runtime), 'run', str(setuid_root/'suid'), '--root', str(setuid_root), '--uid', '1000', '--gid', '1000', *common, *extra], capture_output=True, timeout=60)
        failed += r.returncode != 10
        print(f'{backend} setuid first program: {"PASS" if r.returncode == 10 else "FAIL"} exit={r.returncode} {r.stderr.decode().strip()}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
