#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile

lock=json.loads(Path('/source/sources.lock.json').read_text())
names=['fdk']+(['encoder'] if os.environ.get('AAC_WITH_ENCODER')=='1' else [])
for name in names:
    spec=lock[name];archive=Path('/export/third-party-source')/(name+'-source.tar.gz')
    subprocess.run(['curl','--fail','--location','--retry','3',spec['url'],'--output',str(archive)],check=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest()!=spec['sha256']:
        raise RuntimeError('Source checksum mismatch: '+name)
    target=Path('/work')/name;target.mkdir()
    with tarfile.open(str(archive)) as tar:
        for member in tar.getmembers():
            parts=Path(member.name).parts
            if '..' in parts or member.name.startswith('/') or not (member.isdir() or member.isfile()):
                raise RuntimeError('Unsafe source archive member: '+member.name)
            if len(parts)>1:
                member.name=str(Path(*parts[1:]));tar.extract(member,str(target))
