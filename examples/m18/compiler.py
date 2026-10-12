#!/usr/bin/env python3
import os,sys
args=[]
for a in sys.argv[1:]:
 if a.startswith('-Wp,-MD,'):args.extend(['-MD','-MF',a[len('-Wp,-MD,'):]])
 elif a.startswith('-Wl,-Map,') or a=='-Wl,--verbose':continue
 elif a in ('-Wl,--warn-common','-Wl,--sort-common','-Wl,--sort-section,alignment'):continue
 else:args.append(a)
zig=os.environ['VMFREE_ZIG']
os.execv(zig,[zig,'cc','-target','aarch64-linux-musl','-fno-sanitize=all',*args])
