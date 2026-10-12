#!/usr/bin/env python3
"""M20 bounded pipes, ordered redirection, failures, OCI and optional lifecycle."""
import argparse,hashlib,json,platform,shutil,subprocess,sys,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',type=Path,default=ROOT/'build/vmfree-c');p.add_argument('--report',type=Path);p.add_argument('--lifecycle',action='store_true');a=p.parse_args()
    native=str(a.native.resolve());d=ROOT/'examples/m20';results=[]
    def check(name,passed,**details):
        results.append(dict(name=name,passed=bool(passed),**details));print(('PASS ' if passed else 'FAIL ')+name,flush=True)
        if not passed:raise RuntimeError(str(details))
    try:
        manifest=json.loads((d/'build-manifest.json').read_text())
        check('fixture_hashes',all(hashlib.sha256((ROOT/k).read_bytes()).hexdigest()==v for k,v in manifest['sha256'].items()))
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'root';(root/'bin').mkdir(parents=True)
            for source,name in [(d/'shell.elf','sh'),(d/'probe.elf','probe'),(ROOT/'examples/m18/busybox.elf','busybox')]:shutil.copy(source,root/'bin'/name)
            (root/'hello.txt').write_text('hello file\n')
            outside=Path(tmp)/'outside';outside.write_text('private host file\n');(root/'link').symlink_to(outside)
            def expect(name,command,code=0,out=b'',err=b'',writable=True,flags=(),steps=50000000):
                r=subprocess.run([native,'run',root/'bin/sh','--root',root,'--max-steps',str(steps),*(['--writable'] if writable else []),*flags,'--','-c',command],capture_output=True,timeout=20)
                check(name,r.returncode==code and r.stdout==out and (err is None or r.stderr==err),code=r.returncode,stdout_bytes=len(r.stdout),stdout_prefix=r.stdout[:160].decode(errors='replace'),stderr=r.stderr.decode(errors='replace'))
            cases=[('two_stage','busybox echo hello | busybox cat',0,b'hello\n'),
                ('three_stage','busybox cat /hello.txt | busybox cat | busybox cat',0,b'hello file\n'),
                ('empty_pipeline','true | busybox cat',0,b''),
                ('closed_writer_eof','busybox echo x | busybox cat; busybox echo done',0,b'x\ndone\n'),
                ('conditional_group','false && busybox echo bad | busybox cat || busybox echo good | busybox cat',0,b'good\n'),
                ('failed_producer','probe exit 7 | busybox echo not-run',7,b''),
                ('failed_consumer','busybox echo buffered | probe exit 8',8,b''),
                ('pipeline_exit_is_local','exit 0 | busybox cat; busybox echo continued',0,b'continued\n'),
                ('stdin_default_eof','busybox cat',0,b''),
                ('truncate_append_read','busybox echo alpha >/out; busybox echo beta >>/out; busybox cat </out',0,b'alpha\nbeta\n'),
                ('truncate_existing','busybox echo replacement >/out; busybox cat /out',0,b'replacement\n'),
                ('input_into_pipeline','busybox cat </hello.txt | busybox cat',0,b'hello file\n'),
                ('quoted_operators','busybox echo "a|b > c" >"/quoted file"; busybox cat <"/quoted file"',0,b'a|b > c\n'),
                ('combined_output','probe both >/combined 2>&1; busybox cat /combined',0,b'stdout\nstderr\n'),
                ('redir_order','probe both 2>&1 >/separate; busybox cat /separate',0,b'stderr\nstdout\n'),
                ('separate_stderr','probe both >/stdout 2>/stderr; busybox cat /stdout /stderr',0,b'stdout\nstderr\n'),
                ('stderr_append','probe both >/stdout 2>>/stderr; busybox cat /stderr',0,b'stderr\nstderr\n'),
                ('override_pipe_output','busybox echo stored >/override | busybox cat; busybox cat /override',0,b'stored\n'),
                ('override_pipe_input','busybox echo ignored | busybox cat </hello.txt',0,b'hello file\n'),
                ('builtin_redirect','false; status >/status; busybox cat /status',0,b'1\n'),
                ('exit_redirect','exit 17 >/exit',17,b''),
                ('repeated_pipelines',';'.join(['busybox echo x | busybox cat']*8),0,b'x\n'*8),
                ('medium_pipeline','probe emit | busybox cat',0,b'Z'*(16*4096)),
                ('capacity_boundary','probe full | busybox cat',0,b'Z'*(1024*1024))]
            for name,command,code,out in cases:expect(name,command,code,out)
            expect('dup_syscall_dispatch','probe dup-syscalls')
            expect('builtin_closed_output','status >&-',1)
            expect('capacity_failure','probe overflow | busybox cat',1,err=b'probe: output failed\n')
            expect('closed_self_dup','true 1>&- 1>&1',1,err=b'sh: redirection failed\n')
            expect('closed_output','busybox echo x >&-',1,err=None)
            expect('closed_stderr','probe both 2>&-',out=b'stdout\n')
            expect('console_stderr_dup','probe both 1>&2',out=b'',err=b'stdout\nstderr\n')
            for name,command in [('missing_input','busybox cat </missing'),('readonly_output','busybox echo x >/denied'),('symlink_output','busybox echo x >/link'),('traversal_output','busybox echo x >../outside')]:
                expect(name,command,1,err=b'sh: redirection failed\n',writable=name!='readonly_output')
            check('outside_unchanged',outside.read_text()=='private host file\n' and not (root/'denied').exists())
            for syntax in ['busybox echo should-not-run |','true >','true << /hello.txt','true &','true 2>&9','true 2>&12','true >>> /bad','busybox echo $HOME','true; | false','true 3>/bad','true 12>/bad']:
                expect('reject_'+syntax,syntax,2,err=b'sh: unsupported syntax or command limit\n')
            check('syntax_has_no_side_effects',not (root/'bad').exists())
            for flag in ['--no-decode-cache','--no-lookup-cache']:
                expect('pipeline'+flag,'busybox echo cache | busybox cat > /cached; busybox cat </cached',out=b'cache\n',flags=[flag])
            expect('shared_budget','probe emit | busybox cat',125,err=b'vmfree: Instruction budget exhausted\n',steps=1000)
            def oci(command,writable=False):
                return subprocess.run([sys.executable,ROOT/'oci.py','run',d/'shell-image','--native',native,*(['--writable'] if writable else []),'--','-c',command],capture_output=True,timeout=20)
            r=oci('busybox echo OCI | busybox cat');check('oci_pipe',r.returncode==0 and r.stdout==b'OCI\n' and not r.stderr)
            r=oci('busybox echo temporary >/out; busybox cat </out',True);check('oci_writable',r.returncode==0 and r.stdout==b'temporary\n' and not r.stderr)
            r=oci('busybox cat </out');check('oci_writes_discarded',r.returncode==1 and not r.stdout)
            r=oci('busybox echo denied >/out');check('oci_readonly_default',r.returncode==1 and not r.stdout)
            if a.lifecycle:
                state=Path(tmp)/'state';cli=[sys.executable,ROOT/'containers.py','--state-dir',state,'--native',native]
                def container(*args):
                    r=subprocess.run([*cli,*args],capture_output=True,timeout=15)
                    if r.returncode:raise RuntimeError(r.stderr.decode(errors='replace'))
                    return r.stdout
                container('create','m20',d/'shell-image','--arg=-c','--arg','busybox echo persistent | busybox cat >>/out; probe exit 37')
                for iteration in range(2):
                    container('start','m20');deadline=time.monotonic()+10
                    while True:
                        info=json.loads(container('inspect','m20'))
                        if info['status']['state'] in ('exited','failed'):break
                        if time.monotonic()>deadline:raise RuntimeError('Container did not finish')
                        time.sleep(.05)
                    check('container_status_'+str(iteration),info['status']['exit_code']==37)
                check('container_redirection_persists',(state/'m20/rootfs/out').read_bytes()==b'persistent\npersistent\n')
                container('rm','m20');check('container_removed',not (state/'m20').exists())
    except Exception as exc:
        if not results or results[-1]['passed']:results.append(dict(name='verification_error',passed=False,error=str(exc)));print('FAIL verification_error',str(exc))
    report=dict(milestone=20,host=platform.platform(),passed=bool(results) and all(r['passed'] for r in results),results=results,linux_reference='not-run',runtime_sha256=hashlib.sha256(Path(native).read_bytes()).hexdigest(),source_hashes={str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted((ROOT/'native').glob('*')) if x.suffix in ('.c','.h')})
    if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
    return 0 if report['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
