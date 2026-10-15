#!/usr/bin/env python3
"""Descriptor-anchored working directories and process inheritance."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--native', type=Path, default=ROOT / 'build/dockiva-runtime')
p.add_argument('--report', type=Path, required=True)
p.add_argument('--linux-reference', action='store_true')
a = p.parse_args()
rows = []
for probe,wanted in [('probe',b'cwd inheritance, isolation, shared offsets and boundaries passed\n'),('directory',b'fchdir, shared directory cursor and descriptor lifetime passed\n')]:
    for mode in [[], ['--schedule'], ['--process-tree']]:
        for cache in [[], ['--no-decode-cache'], ['--no-lookup-cache']]:
            with tempfile.TemporaryDirectory() as temp:
                root = Path(temp) / 'root';root.mkdir()
                (root / 'escape').symlink_to(Path(temp))
                r = subprocess.run([str(a.native.resolve()), 'run', str(ROOT / ('examples/m27/'+probe+'.elf')),
                    '--root', str(root), '--writable', *mode, *cache], capture_output=True, timeout=10)
                ok = r.returncode == 0 and r.stdout == wanted and not r.stderr
                rows.append(dict(probe=probe, mode=mode, cache=cache, passed=ok, exit_code=r.returncode,
                                 stdout=r.stdout.decode(), stderr=r.stderr.decode()))
                print(('PASS' if ok else 'FAIL'), mode, cache, r.returncode, flush=True)
with tempfile.TemporaryDirectory() as temp:
    root=Path(temp);(root/'a').mkdir();(root/'a/b').mkdir();(root/'bin').mkdir()
    for name in ['sh','cat']:shutil.copy2(ROOT/'examples/m24/busybox.elf',root/'bin'/name)
    script='cd /a; pwd; (cd b; pwd); pwd; echo hi >data; cat data'
    r=subprocess.run([str(a.native.resolve()),'run',str(ROOT/'examples/m24/busybox.elf'),'--root',temp,
        '--writable','--process-tree','--max-steps','10000000','--env','PATH=/bin','--','sh','-c',script],capture_output=True,timeout=10)
    rows.append(dict(name='upstream_ash_cd',passed=r.returncode==0 and r.stdout==b'/a\n/a/b\n/a\nhi\n' and not r.stderr,
        exit_code=r.returncode,stdout=r.stdout.decode(),stderr=r.stderr.decode()))
    print(rows[-1],flush=True)
if a.linux_reference:
    with tempfile.TemporaryDirectory() as temp:
        root=Path(temp)/'root';shutil.copytree(ROOT/'examples/m28/root',root);shutil.copy2(ROOT/'examples/m27/directory.elf',root/'bin/directory-probe')
        image=json.loads((ROOT/'examples/m28/build-manifest.json').read_text())['image_id']
        r=subprocess.run(['docker','run','--rm','--network=none','--read-only','--cap-drop=ALL','--cap-add=SYS_CHROOT','-v',str(root)+':/fixture','--entrypoint=/bin/busybox',image,'chroot','/fixture','/bin/directory-probe'],capture_output=True,timeout=15)
        rows.append(dict(name='linux_directory_cursor',passed=r.returncode==0 and r.stdout==b'fchdir, shared directory cursor and descriptor lifetime passed\n' and not r.stderr,exit_code=r.returncode,stdout=r.stdout.decode(),stderr=r.stderr.decode()))
        print(rows[-1],flush=True)
a.report.parent.mkdir(parents=True,exist_ok=True)
a.report.write_text(json.dumps(dict(passed=all(r['passed'] for r in rows),checks=rows,
    runtime_sha256=hashlib.sha256(a.native.read_bytes()).hexdigest()),indent=2)+'\n')
raise SystemExit(0 if all(r['passed'] for r in rows) else 1)
