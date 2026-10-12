#!/usr/bin/env python3
"""Verified local OCI image cache and scoped Distribution API pulls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import oci
INDEX='application/vnd.oci.image.index.v1+json'
DEFAULT=Path.home()/'.vmfree/images'

def key(name):return hashlib.sha256(name.encode()).hexdigest()
def resolve(cache,name):
    folder=Path(cache)/key(name)
    if folder.is_symlink():raise ValueError('Image cache symlink rejected')
    if not folder.is_dir():raise ValueError('Image is not cached: '+name)
    return folder/'layout'

def private(cache):
    cache=Path(cache).expanduser().absolute()
    if cache.is_symlink():raise ValueError('Cache symlink rejected')
    cache.mkdir(parents=True,exist_ok=True,mode=0o700)
    if cache.stat().st_uid!=os.getuid() or cache.stat().st_mode&0o077:raise ValueError('Cache must be owned by you with mode 0700')
    return cache

def commit(cache,name,layout):
    cache=private(cache);image=oci.Image(layout)
    target=cache/key(name)
    if target.exists():
        old=oci.Image(resolve(cache,name))
        if old.digest!=image.digest:raise ValueError('Cached name already points to another digest; use a new name')
        return target/'layout'
    with tempfile.TemporaryDirectory(prefix='.incoming-',dir=cache) as tmp:
        stage=Path(tmp)/'item';stage.mkdir(mode=0o700)
        dest=stage/'layout';(dest/'blobs/sha256').mkdir(parents=True)
        source=Path(layout).resolve()
        raw=oci.local_bytes(source,'blobs/sha256/'+image.digest[7:],oci.MAX_JSON)
        if oci.sha(raw)!=image.digest:raise ValueError('Manifest changed during import')
        manifest=oci.json_object(raw)
        (dest/'blobs/sha256'/image.digest[7:]).write_bytes(raw)
        for desc in [manifest['config'],*manifest['layers']]:
            data=oci.blob(source,desc,{oci.CONFIG,oci.TAR,oci.TAR+'+gzip'})
            (dest/'blobs/sha256'/desc['digest'][7:]).write_bytes(data)
        (dest/'oci-layout').write_text('{"imageLayoutVersion":"1.0.0"}')
        (dest/'index.json').write_text(json.dumps(dict(schemaVersion=2,manifests=[dict(mediaType=oci.MANIFEST,digest=image.digest,size=len(raw))])))
        (stage/'image.json').write_text(json.dumps(dict(name=name,digest=image.digest)))
        os.rename(stage,target)
    return target/'layout'

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,*args,**kwargs):raise ValueError('Registry redirects unsupported; credentials were not forwarded')

def pull(reference,cache=DEFAULT,insecure_local=False,token=None):
    if not isinstance(reference,str) or len(reference)>512:raise ValueError('Invalid registry reference length')
    if token is not None and (not isinstance(token,str) or not token or len(token)>16384 or '\n' in token or '\r' in token):raise ValueError('Invalid registry token format')
    if '/' not in reference:raise ValueError('Use explicit registry/repository:tag or @sha256:digest')
    host,rest=reference.split('/',1)
    if not re.fullmatch(r'[A-Za-z0-9.-]+(?::[0-9]{1,5})?',host):raise ValueError('Invalid registry host')
    if '@' in rest:repo,ref=rest.rsplit('@',1)
    elif ':' in rest:repo,ref=rest.rsplit(':',1)
    else:repo,ref=rest,'latest'
    if not re.fullmatch(r'[a-z0-9]+(?:[._-][a-z0-9]+)*(?:/[a-z0-9]+(?:[._-][a-z0-9]+)*)*',repo):raise ValueError('Invalid repository')
    if not (oci.DIGEST.fullmatch(ref) or re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}',ref)):raise ValueError('Invalid tag/digest')
    if insecure_local and host.split(':')[0]!='127.0.0.1':raise ValueError('Plain HTTP is only allowed for literal 127.0.0.1')
    origin=('http://' if insecure_local else 'https://')+host
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}),NoRedirect())
    used=0
    def fetch(path,limit,authenticate=True):
        nonlocal token,used
        headers={'Accept':', '.join([oci.MANIFEST,INDEX]),'Accept-Encoding':'identity'}
        if token and authenticate:headers['Authorization']='Bearer '+token
        url=origin+path
        try:response=opener.open(urllib.request.Request(url,headers=headers),timeout=15)
        except urllib.error.HTTPError as e:
            challenge=e.headers.get('WWW-Authenticate','')
            if e.code!=401 or token or not challenge.lower().startswith('bearer '):raise ValueError('Registry request failed with HTTP '+str(e.code)) from None
            fields=dict(re.findall(r'([a-zA-Z]+)="([^"]*)"',challenge))
            realm=fields.get('realm','');parsed=urllib.parse.urlsplit(realm)
            if parsed.scheme!='https' or not parsed.hostname or parsed.username or parsed.password or parsed.fragment:raise ValueError('Invalid HTTPS token realm')
            query=urllib.parse.urlencode(dict(service=fields.get('service',''),scope='repository:'+repo+':pull'))
            with opener.open(realm+('&' if parsed.query else '?')+query,timeout=15) as r:
                data=r.read(oci.MAX_JSON+1)
            auth=oci.json_object(data);token=auth.get('token',auth.get('access_token'))
            if not isinstance(token,str) or not token or len(token)>16384 or '\n' in token or '\r' in token:raise ValueError('Invalid registry token')
            return fetch(path,limit)
        with response:
            data=response.read(limit+1);digest=response.headers.get('Docker-Content-Digest')
        if len(data)>limit:raise ValueError('Registry object exceeds limit')
        used+=len(data)
        if used>128*1024*1024:raise ValueError('Pull exceeds 128 MiB download budget')
        if digest and digest!=oci.sha(data):raise ValueError('Registry digest header mismatch')
        return data
    with tempfile.TemporaryDirectory(prefix='vmfree-pull-') as tmp:
        layout=Path(tmp);blobs=layout/'blobs/sha256';blobs.mkdir(parents=True)
        def store(data,media):
            digest=oci.sha(data);(blobs/digest[7:]).write_bytes(data)
            return dict(digest=digest,size=len(data),mediaType=media)
        data=fetch('/v2/'+repo+'/manifests/'+ref,oci.MAX_JSON)
        if ref.startswith('sha256:') and oci.sha(data)!=ref:raise ValueError('Requested manifest digest mismatch')
        manifest=oci.json_object(data)
        if manifest.get('mediaType')==INDEX:
            items=manifest.get('manifests',[])
            if not isinstance(items,list) or len(items)>32:raise ValueError('Invalid image index')
            candidates=[d for d in items if isinstance(d,dict) and isinstance(d.get('platform'),dict) and d['platform'].get('os')=='linux' and d['platform'].get('architecture')=='arm64']
            if len(candidates)!=1:raise ValueError('Index must select exactly one linux/arm64 image')
            desc=candidates[0]
            if not oci.DIGEST.fullmatch(str(desc.get('digest',''))):raise ValueError('Invalid index digest')
            data=fetch('/v2/'+repo+'/manifests/'+desc['digest'],oci.MAX_JSON)
            if oci.sha(data)!=desc['digest'] or len(data)!=desc.get('size'):raise ValueError('Selected manifest descriptor mismatch')
            manifest=oci.json_object(data)
        if manifest.get('mediaType')!=oci.MANIFEST:raise ValueError('Only OCI image manifests supported')
        descriptor=store(data,oci.MANIFEST)
        layers=manifest.get('layers',[])
        if not isinstance(layers,list) or len(layers)>32:raise ValueError('Too many layers')
        for d in [manifest.get('config'),*layers]:
            if not isinstance(d,dict) or not oci.DIGEST.fullmatch(str(d.get('digest',''))) or type(d.get('size')) is not int or not 0<=d['size']<=oci.MAX_BLOB:raise ValueError('Invalid blob descriptor')
            raw=fetch('/v2/'+repo+'/blobs/'+d['digest'],d['size'])
            if len(raw)!=d['size'] or oci.sha(raw)!=d['digest']:raise ValueError('Downloaded blob digest/size mismatch')
            store(raw,d.get('mediaType'))
        (layout/'oci-layout').write_text('{"imageLayoutVersion":"1.0.0"}')
        (layout/'index.json').write_text(json.dumps(dict(schemaVersion=2,manifests=[descriptor])))
        return commit(cache,reference,layout)

def main():
    os.umask(0o077)
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cache',type=Path,default=DEFAULT)
    sub=p.add_subparsers(dest='action',required=True)
    q=sub.add_parser('import');q.add_argument('name');q.add_argument('layout',type=Path)
    q=sub.add_parser('pull');q.add_argument('reference');q.add_argument('--insecure-local',action='store_true');q.add_argument('--token-env')
    q=sub.add_parser('inspect');q.add_argument('name')
    sub.add_parser('list');a=p.parse_args()
    if a.action=='import':print(commit(a.cache,a.name,a.layout))
    elif a.action=='pull':print(pull(a.reference,a.cache,a.insecure_local,os.environ.get(a.token_env) if a.token_env else None))
    elif a.action=='inspect':
        image=oci.Image(resolve(a.cache,a.name));print(json.dumps(dict(name=a.name,digest=image.digest,command=image.command()[0]),indent=2))
    else:
        cache=private(a.cache);print(json.dumps([json.loads(p.read_text()) for p in sorted(cache.glob('*/image.json'))],indent=2))
if __name__=='__main__':
    try:main()
    except (OSError,ValueError) as e:raise SystemExit('image error: '+str(e))
