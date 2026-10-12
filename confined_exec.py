#!/usr/bin/env python3
"""Apply portable host resource limits, then replace this process with the runtime."""
import os
import resource
import sys

def main():
    arguments=sys.argv[1:]
    sandbox=bool(arguments and arguments[0]=='--seatbelt')
    if sandbox:arguments=arguments[1:]
    if not arguments:raise SystemExit('Expected executable')
    if sandbox:
        from host_sandbox import command
        arguments=command(arguments)
        # Only standard I/O capabilities may survive entry into the policy.
        os.closerange(3,resource.getrlimit(resource.RLIMIT_NOFILE)[0])
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    resource.setrlimit(resource.RLIMIT_FSIZE,(16*1024*1024,16*1024*1024))
    resource.setrlimit(resource.RLIMIT_NOFILE,(128,128))
    resource.setrlimit(resource.RLIMIT_CPU,(60,60))
    os.execv(arguments[0],arguments)
if __name__=='__main__':
    try:main()
    except (OSError,ValueError) as exc:
        print('vmfree launcher: '+str(exc),file=sys.stderr);raise SystemExit(125)
