#!/usr/bin/env python3
"""Test opt-in ELF input bounds and resource-limit enforcement."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temp:
    binary=Path(temp)/'large.elf'
    binary.write_bytes((ROOT/'examples/m18/busybox.elf').read_bytes())
    with binary.open('r+b') as stream:stream.truncate(17*1024*1024)
    for runtime in ['vmfree-c','vmfree-c-sanitize']:
        def check(options,good):
            p=subprocess.run([str(ROOT/'build'/runtime),'run',str(binary),'--argv0','busybox',*options,'--','echo','bounded'],capture_output=True,timeout=15)
            assert (p.returncode==0 and p.stdout==b'bounded\n' and not p.stderr) if good else (p.returncode==125 and not p.stdout), (runtime,options,p.returncode,p.stdout,p.stderr)
            print('PASS',runtime,options)
        check([],False)
        check(['--elf-limit-mib','64'],True)
        check(['--elf-limit-mib','64','--memory-limit-mib','16'],False)
        for option,values in [('--elf-limit-mib',['15','65','-1','x']),('--mmap-limit-mib',['15','2049','-1','x'])]:
            for value in values:check([option,value],False)
        with binary.open('r+b') as stream:stream.truncate(64*1024*1024+1)
        check(['--elf-limit-mib','64'],False)
        with binary.open('r+b') as stream:stream.truncate(17*1024*1024)
