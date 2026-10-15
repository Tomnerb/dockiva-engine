#!/usr/bin/env python3
"""Validate bounded guest processes, shell syntax, exec recovery and OCI execution."""
import argparse,hashlib,json,platform,shutil,subprocess,sys,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',type=Path,default=ROOT/'build/dockiva-runtime');p.add_argument('--report',type=Path);p.add_argument('--lifecycle',action='store_true');a=p.parse_args()
    native=str(a.native.resolve());d=ROOT/'examples/m19';results=[]
    def check(name,passed,**detail):
        results.append(dict(name=name,passed=bool(passed),**detail));print(('PASS ' if passed else 'FAIL ')+name,flush=True)
        if not passed:raise RuntimeError(str(detail))
    try:
        manifest=json.loads((d/'build-manifest.json').read_text())
        check('fixture_hashes',all(hashlib.sha256((ROOT/k).read_bytes()).hexdigest()==v for k,v in manifest['sha256'].items()))
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'root';(root/'bin').mkdir(parents=True)
            for source,name in [(d/'shell.elf','sh'),(d/'probe.elf','probe'),(ROOT/'examples/m18/busybox.elf','busybox')]:shutil.copy(source,root/'bin'/name)
            (root/'hello.txt').write_text('hello file\n');(root/'letters').write_text('abc')
            (root/'bad').write_bytes(b'not ELF');(root/'bad').chmod(0o755)
            (root/'noexec').write_bytes((d/'probe.elf').read_bytes());(root/'noexec').chmod(0o644)
            (root/'link').symlink_to(root/'bin/probe');(root/'dirlink').symlink_to(root/'bin',target_is_directory=True)
            malformed=bytearray((d/'probe.elf').read_bytes());malformed[24:32]=(0).to_bytes(8,'little')
            (root/'badentry').write_bytes(malformed);(root/'badentry').chmod(0o755)
            def run(binary,args,flags=(),steps=10000000):
                return subprocess.run([native,'run',root/'bin'/binary,'--root',root,'--max-steps',str(steps),'--env','M19=value',*flags,'--',*args],capture_output=True,timeout=20)
            def expect(name,binary,args,code=0,out=b'',err=None,**kw):
                r=run(binary,args,**kw);check(name,r.returncode==code and r.stdout==out and (err is None or err in r.stderr),code=r.returncode,stdout=r.stdout.decode(errors='replace'),stderr=r.stderr.decode(errors='replace'))
            expect('clone_memory_pid_wait','probe',['fork'],out=b'fork isolation and wait status OK\n')
            expect('shared_file_offset_private_fd_table','probe',['fd-fork'])
            expect('exec_descriptor_cloexec','probe',['fds'])
            for path,errno in [('/missing',2),('/bad',8),('/badentry',8),('/noexec',13),('/link',40),('/dirlink/probe',20),('/../outside',13)]:
                expect('exec_recovery_'+path,'probe',['exec-error',path,str(errno)],out=b'exec preserved caller\n')
            expect('exec_limit','probe',['exec-loop'],code=11)
            cases=[('sequence',"busybox echo 'hello world'; probe exit 7; status; busybox cat /hello.txt",0,b'hello world\n7\nhello file\n'),
                   ('conditional','false && busybox echo bad || busybox echo good; true || busybox echo bad',0,b'good\n'),
                   ('quotes','busybox echo "two words" empty\\ word \'\' \'$literal\'',0,b'two words empty word  $literal\n'),
                   ('exit','probe exit 23; exit',23,b''),('explicit_exit','exit 9; busybox echo bad',9,b''),
                   ('missing','missing',127,b''),('exec_error','/bad',126,b''),('environment','probe env',0,b'M19=value\n'),
                   ('empty','',0,b'')]
            for name,command,code,out in cases:
                expect('shell_'+name,'sh',['-c',command],code,out)
            for flag in ['--no-decode-cache','--no-lookup-cache']:
                expect('clone'+flag,'probe',['fork'],out=b'fork isolation and wait status OK\n',flags=[flag])
                expect('shell'+flag,'sh',['-c',cases[0][1]],out=cases[0][3],flags=[flag])
            for syntax in ['busybox echo should-not-run | busybox cat','true &','true > /out','echo $HOME',"echo 'unterminated",'true &&','true; ; false']:
                expect('syntax_rejected_'+syntax,'sh',['-c',syntax],2,err=b'unsupported syntax')
            expect('child_count_bound','sh',['-c',';'.join(['probe exit 0']*17)],125,err=b'child limit')
            expect('shared_instruction_bound','sh',['-c','probe spin'],125,err=b'Instruction budget exhausted',steps=100000)
            r=subprocess.run([sys.executable,ROOT/'oci.py','run',d/'shell-image','--native',native,'--','-c','busybox echo OCI; probe exit 19'],capture_output=True,timeout=20)
            check('oci_shell_status',r.returncode==19 and r.stdout==b'OCI\n',code=r.returncode,stderr=r.stderr.decode(errors='replace'))
            if a.lifecycle:
                state=Path(tmp)/'state';cli=[sys.executable,ROOT/'containers.py','--state-dir',state,'--native',native]
                def container(*args):
                    r=subprocess.run([*cli,*args],capture_output=True,timeout=15)
                    if r.returncode:raise RuntimeError(r.stderr.decode(errors='replace'))
                    return r.stdout
                container('create','m19',d/'shell-image','--arg=-c','--arg','busybox echo lifecycle; probe exit 37')
                for iteration in range(2):
                    container('start','m19');deadline=time.monotonic()+10
                    while True:
                        info=json.loads(container('inspect','m19'))
                        if info['status']['state'] in ('exited','failed'):break
                        if time.monotonic()>deadline:raise RuntimeError('Container did not finish')
                        time.sleep(.05)
                    check('container_status_'+str(iteration),info['status']['exit_code']==37)
                check('container_logs',b'lifecycle' in container('logs','m19'))
                container('rm','m19');check('container_removed',not (state/'m19').exists())
    except Exception as exc:
        if not results or results[-1]['passed']:results.append(dict(name='verification_error',passed=False,error=str(exc)))
    report=dict(milestone=19,host=platform.platform(),passed=bool(results) and all(r['passed'] for r in results),results=results,linux_reference='not-run',runtime_sha256=hashlib.sha256(Path(native).read_bytes()).hexdigest(),source_hashes={str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted((ROOT/'native').glob('*')) if x.suffix in ('.c','.h')})
    if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
    return 0 if report['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
