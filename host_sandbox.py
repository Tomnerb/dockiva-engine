"""Experimental macOS Seatbelt launcher policy; fail closed when requested.

This is a local qualification candidate, not an independently reviewed boundary.
"""
import os
from pathlib import Path
import platform

def literal(value):
    value=str(value)
    if any(ord(c)<32 or ord(c)>126 for c in value):raise ValueError('Sandbox paths must be printable ASCII')
    return '"'+value.replace('\\','\\\\').replace('"','\\"')+'"'

def command(arguments):
    if platform.system()!='Darwin':raise ValueError('The requested host sandbox is currently macOS-only')
    if len(arguments)<3 or arguments[1]!='run':raise ValueError('Host sandbox requires a native run command')
    executable=Path(arguments[0]).resolve(strict=True);guest=Path(arguments[2]).resolve(strict=True)
    options=arguments[3:arguments.index('--')] if '--' in arguments[3:] else arguments[3:]
    if '--root' not in options:raise ValueError('Host sandbox requires an explicit guest root')
    index=options.index('--root')
    if index+1>=len(options):raise ValueError('Missing guest root')
    root=Path(options[index+1]).resolve(strict=True)
    if not root.is_dir() or root==Path('/'):raise ValueError('Invalid guest root for host sandbox')
    policy=['(version 1)','(deny default)',
        '(allow file-read* (literal "/") (literal "/System") (literal "/System/Volumes") (literal "/System/Volumes/Preboot") (literal "/System/Volumes/Preboot/Cryptexes") (literal "/System/Volumes/Preboot/Cryptexes/OS") (literal "/System/Cryptexes") (literal "/System/Cryptexes/OS"))',
        '(allow process-exec (literal '+literal(executable)+'))',
        '(allow sysctl-read)',
        '(allow file-read* (subpath "/usr/lib") (subpath "/System/Library") (subpath "/System/Volumes/Preboot/Cryptexes/OS/System/Library") (subpath "/System/Volumes/Preboot/Cryptexes/OS/usr/lib") (literal '+literal(executable)+') (literal '+literal(guest)+') (subpath '+literal(root)+'))',
        '(allow file-read* file-write* (literal "/dev/null"))',
        '(allow network-outbound (remote tcp "localhost:*"))']
    if '--writable' in options:policy.append('(allow file-write* (subpath '+literal(root)+'))')
    parents=set(executable.parents)|set(guest.parents)|set(root.parents)
    policy.append('(allow file-read-metadata '+' '.join('(literal '+literal(x)+')' for x in sorted(parents))+')')
    normalized=list(arguments);normalized[0]=str(executable);normalized[2]=str(guest)
    normalized[3+index+1]=str(root)
    return ['/usr/bin/sandbox-exec','-p','\n'.join(policy),*normalized]
