#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

root=Path('/export')
with_encoder=os.environ.get('AAC_WITH_ENCODER')=='1'
required=['aac_decoder_probe']+(['aac_fdk_plugin'] if with_encoder else [])
for name in required:
    if not (root/(name+'.dvcp.bundle')/'Contents/Linux-x86-64'/(name+'.dvcp')).is_file():
        raise RuntimeError('Required plugin missing: '+name)
elfs=list(root.glob('*.dvcp.bundle/Contents/Linux-x86-64/*.dvcp'))+[root/'tools/check-resolve']
abi={}
for path in elfs:
    versions=subprocess.check_output(['readelf','--version-info',str(path)],universal_newlines=True)
    glibc=max(tuple(map(int,v.split('.'))) for v in re.findall(r'Name: GLIBC_([0-9.]+)',versions))
    glibcxx=max(tuple(map(int,v.split('.'))) for v in re.findall(r'Name: GLIBCXX_([0-9.]+)',versions))
    if glibc>(2,28) or glibcxx>(3,4,25) or 'GLIBC_ABI_' in versions:
        raise RuntimeError('Unexpected newer runtime requirement: '+str(path))
    dynamic=subprocess.check_output(['readelf','-d',str(path)],universal_newlines=True)
    needed=re.findall(r'Shared library: \[(.*?)\]',dynamic)
    if any('avcodec' in x or 'avformat' in x or 'fdk' in x for x in needed):
        raise RuntimeError('Codec is not self-contained: '+str(path))
    abi[str(path.relative_to(root))]={'glibc':'.'.join(map(str,glibc)),
        'glibcxx':'.'.join(map(str,glibcxx)),'needed':needed}
sdk=Path('/sdk')
record={'schema':1,'version':'0.2.0-experimental','platform':'linux-x86_64',
    'features':['aac-decoding']+(['aac-encoding'] if with_encoder else []),
    'decode_containers':['mp4','mov','mkv'],
    'source_sha256':os.environ['AAC_SOURCE_SHA256'],
    'compiler':subprocess.check_output(['g++','--version'],universal_newlines=True).splitlines()[0],
    'sdk_sha256':{str(p.relative_to(sdk)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(sdk.rglob('*')) if p.is_file()},
    'abi':abi,'tests':['AAC metadata and malformed/truncated headers','AAC normal/missing ASC',
        'empty/full probe','flush/replay PCM equality','bounded end-of-stream drain and termination'],
    'tested_resolve':'Studio 21.0.4.0005 Linux x86-64 (other versions require validation)'}
(root/'BUILD.json').write_text(json.dumps(record,indent=2)+'\n')
lines=[hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p.relative_to(root)) for p in sorted(root.rglob('*')) if p.is_file()]
(root/'SHA256SUMS').write_text('\n'.join(lines)+'\n')
print('PASS: Linux ABI <= GLIBC 2.28 / GLIBCXX 3.4.25; no external codec dependency')
