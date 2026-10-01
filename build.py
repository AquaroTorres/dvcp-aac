#!/usr/bin/env python3
"""Build portable Linux x86-64 AAC decoder and encoder plugins in a disposable container."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tarfile
import tempfile
import uuid

HERE=Path(__file__).resolve().parent
SDK_FILES=['include/IOPluginDefs.h','include/IOPluginProps.h','wrapper/host_api.h',
           'wrapper/host_api.cpp','wrapper/plugin_api.h','wrapper/plugin_api.cpp']

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk',type=Path,default=Path('/opt/resolve/Developer/CodecPlugin/Examples/x264_encoder_plugin'))
    ap.add_argument('--output',type=Path,default=HERE.parent/'aac-plugin-release')
    ap.add_argument('--runtime',choices=['podman','docker'])
    ap.add_argument('--jobs',type=int,default=min(os.cpu_count() or 4,8))
    scope=ap.add_mutually_exclusive_group()
    scope.add_argument('--with-encoder',dest='with_encoder',action='store_true',help='Build both plugins (default)')
    scope.add_argument('--decode-only',dest='with_encoder',action='store_false',help='Omit the AAC export plugin')
    ap.set_defaults(with_encoder=True)
    a=ap.parse_args();output=a.output.resolve()
    archive=output.with_name(output.name+'.tar.gz')
    if output==HERE or HERE in output.parents or output in HERE.parents:
        ap.error('Choose an output directory separate from the source tree')
    if output.exists():ap.error('Output already exists; select a new output directory')
    if archive.exists() or archive.with_name(archive.name+'.sha256').exists():ap.error('Output archive already exists')
    if not 1<=a.jobs<=64:ap.error('--jobs must be between 1 and 64')
    for name in SDK_FILES:
        if not (a.sdk/name).is_file():ap.error('Missing Resolve SDK file: '+str(a.sdk/name))
    runtime=None
    for candidate in ([a.runtime] if a.runtime else ['podman','docker']):
        if shutil.which(candidate) and subprocess.run([candidate,'info'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0:
            runtime=candidate;break
    if not runtime:ap.error('A working Podman or Docker Linux engine is required')
    lock=json.loads((HERE/'sources.lock.json').read_text());image=lock['image']
    if '@sha256:' not in image:raise RuntimeError('Builder image must be pinned')
    prior_image=subprocess.run([runtime,'image','inspect',image],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0
    output.parent.mkdir(parents=True,exist_ok=True)
    name='resolve-aac-plugin-'+uuid.uuid4().hex[:12]
    def interrupted(signum,frame):raise KeyboardInterrupt()
    previous={s:signal.signal(s,interrupted) for s in (signal.SIGINT,signal.SIGTERM)}
    try:
        with tempfile.TemporaryDirectory(prefix='.aac-build-',dir=str(output.parent)) as tmp:
            tmp=Path(tmp);source=tmp/'source';sdk=tmp/'sdk'
            shutil.copytree(HERE,source,ignore=shutil.ignore_patterns('.git','__pycache__','*.pyc','*.log','build','out','.build-*'))
            for rel in SDK_FILES:
                target=sdk/rel;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(a.sdk/rel,target)
            fingerprint=hashlib.sha256()
            for p in sorted(source.rglob('*')):
                if p.is_file():fingerprint.update(str(p.relative_to(source)).encode()+b'\0'+p.read_bytes())
            def run(args):subprocess.run([str(x) for x in args],check=True)
            try:
                if not prior_image:run([runtime,'pull','--platform','linux/amd64',image])
                run([runtime,'create','--name',name,'--platform','linux/amd64',
                    '-e','AAC_BUILD_JOBS='+str(a.jobs),'-e','AAC_SOURCE_SHA256='+fingerprint.hexdigest(),
                    '-e','AAC_WITH_ENCODER='+str(int(a.with_encoder)),
                    '-v',str(source)+':/source:ro,Z','-v',str(sdk)+':/sdk:ro,Z',
                    image,'bash','/source/scripts/container-build.sh'])
                run([runtime,'start','--attach',name])
                exported=tmp/'export';exported.mkdir()
                run([runtime,'cp',name+':/export/.',exported])
                for line in (exported/'SHA256SUMS').read_text().splitlines():
                    digest,rel=line.split('  ',1);path=exported/rel
                    if Path(rel).is_absolute() or '..' in Path(rel).parts or path.is_symlink():raise RuntimeError('Invalid output path')
                    if hashlib.sha256(path.read_bytes()).hexdigest()!=digest:raise RuntimeError('Output checksum mismatch: '+rel)
                packed=tmp/'release.tar.gz'
                with tarfile.open(str(packed),'w:gz') as tar:tar.add(str(exported),arcname=output.name)
                digest=hashlib.sha256(packed.read_bytes()).hexdigest()
                os.replace(exported,output);os.replace(packed,archive)
                archive.with_name(archive.name+'.sha256').write_text(digest+'  '+archive.name+'\n')
            finally:
                subprocess.run([runtime,'rm','--force',name],stdout=subprocess.DEVNULL)
                if not prior_image:
                    result=subprocess.run([runtime,'image','rm',image],stdout=subprocess.DEVNULL)
                    if result.returncode:print('Builder image is still in use; could not remove it.')
    finally:
        for s,handler in previous.items():signal.signal(s,handler)
    print('Package: '+str(output));print('Share: '+str(archive))

if __name__=='__main__':main()
