#!/usr/bin/env python3
"""Check, install, or undo installation of the packaged AAC plugins."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import time
import uuid

HERE=Path(__file__).resolve().parent
NAMES=['aac_decoder_probe','aac_fdk_plugin']

def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()

def stopped(root):
    for item in Path('/proc').iterdir():
        if not item.name.isdigit():continue
        try:exe=os.readlink(item/'exe')
        except (FileNotFoundError,PermissionError,ProcessLookupError):continue
        if exe.endswith(' (deleted)'):exe=exe[:-10]
        if Path(exe).resolve()==root/'bin/resolve':
            raise RuntimeError('Save and fully quit Resolve first (PID '+item.name+').')

def verify():
    manifest=HERE/'SHA256SUMS'
    if not manifest.is_file():raise RuntimeError('Run the installer from the built release package')
    for line in manifest.read_text().splitlines():
        expected,rel=line.split('  ',1);path=HERE/rel
        if Path(rel).is_absolute() or '..' in Path(rel).parts or path.is_symlink():
            raise RuntimeError('Invalid package path')
        if digest(path)!=expected:raise RuntimeError('Package checksum mismatch: '+rel)

def save(path,state):
    tmp=path.with_suffix('.tmp');tmp.write_text(json.dumps(state,indent=2)+'\n');os.replace(tmp,path)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('action',choices=['check','install','uninstall'])
    ap.add_argument('--root',type=Path,default=Path('/opt/resolve'))
    a=ap.parse_args();root=a.root.resolve();statefile=root/'.aac-plugin-install.json'
    if platform.system()!='Linux' or platform.machine() not in ('x86_64','amd64'):
        raise RuntimeError('This package targets Linux x86-64')
    if a.action=='uninstall':
        stopped(root)
        if not statefile.is_file():raise RuntimeError('No installation record at '+str(statefile))
        state=json.loads(statefile.read_text())
        for record in state['plugins']:
            allowed={'IOPlugins/'+name+'.dvcp.bundle':'Contents/Linux-x86-64/'+name+'.dvcp' for name in NAMES}
            if allowed.get(record['path'])!=record['binary']:
                raise RuntimeError('Invalid installation record')
            if record['backup'] and root/'.aac-plugin-backups' not in Path(record['backup']).resolve().parents:
                raise RuntimeError('Invalid backup path')
            target=root/record['path'];binary=target/record['binary']
            if target.is_symlink() or digest(binary)!=record['sha256']:
                raise RuntimeError('Installed plugin changed; refusing to remove '+str(target))
            if record['backup'] and not Path(record['backup']).is_dir():
                raise RuntimeError('Previous plugin backup is missing')
        for record in reversed(state['plugins']):
            target=root/record['path'];shutil.rmtree(target)
            if record['backup']:os.replace(record['backup'],target)
        previous=state.get('previous_state')
        if previous:save(statefile,previous)
        else:statefile.unlink()
        print('Undid the last AAC plugin installation; application files unchanged.')
        return
    verify()
    if not (root/'bin/resolve').is_file():raise RuntimeError('Resolve executable not found')
    subprocess.run([str(HERE/'tools/check-resolve'),str(root/'bin/resolve')],check=True)
    if a.action=='check':
        print('Package checksums and Resolve bridge patterns passed. Other Resolve versions still require playback testing.')
        return
    stopped(root)
    plugins=root/'IOPlugins';plugins.mkdir(exist_ok=True)
    records=[];previous=json.loads(statefile.read_text()) if statefile.exists() else None
    backupdir=root/'.aac-plugin-backups'/(time.strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex[:8])
    with tempfile.TemporaryDirectory(prefix='.aac-plugin-',dir=str(root)) as tmp:
        tmp=Path(tmp)
        for name in NAMES:
            bundle=name+'.dvcp.bundle';source=HERE/bundle
            if not source.is_dir():continue
            target=plugins/bundle
            if target.is_symlink():raise RuntimeError('Refusing to replace a symlink: '+str(target))
            shutil.copytree(source,tmp/bundle)
        stopped(root)
        try:
            for name in NAMES:
                bundle=name+'.dvcp.bundle';staged=tmp/bundle
                if not staged.exists():continue
                target=plugins/bundle;backup=None
                if target.exists():
                    backupdir.mkdir(parents=True,exist_ok=True);backup=backupdir/bundle
                    os.replace(target,backup)
                binary='Contents/Linux-x86-64/'+name+'.dvcp'
                record={'path':'IOPlugins/'+bundle,'binary':binary,
                        'backup':str(backup) if backup else None,'sha256':digest(staged/binary)}
                records.append(record);os.replace(staged,target)
            save(statefile,{'version':1,'plugins':records,'previous_state':previous})
        except BaseException:
            for record in reversed(records):
                target=root/record['path']
                if target.exists():shutil.rmtree(target)
                if record['backup']:os.replace(record['backup'],target)
            raise
    print('Installed AAC plugin(s). Relaunch Resolve. Its executable, bundled libraries, and media were not changed.')

if __name__=='__main__':
    try:main()
    except (RuntimeError,OSError,subprocess.CalledProcessError,ValueError) as error:
        raise SystemExit('ERROR: '+str(error))
