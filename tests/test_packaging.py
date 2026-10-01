#!/usr/bin/env python3
"""Exercise installer preservation/refusal and disposable-builder cleanup."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result

class Installer(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='aac-installer-test-')
        self.addCleanup(self.tmp.cleanup)
        base=Path(self.tmp.name);self.package=base/'package';self.root=base/'resolve'
        self.rel=Path('aac_decoder_probe.dvcp.bundle/Contents/Linux-x86-64/aac_decoder_probe.dvcp')
        binary=self.package/self.rel;binary.parent.mkdir(parents=True);binary.write_bytes(b'new plugin')
        checker=self.package/'tools/check-resolve';checker.parent.mkdir();checker.write_text('#!/bin/sh\nexit 0\n');checker.chmod(0o755)
        self.target=self.root/'IOPlugins'/self.rel;self.target.parent.mkdir(parents=True);self.target.write_bytes(b'previous plugin')
        (self.root/'bin').mkdir();(self.root/'bin/resolve').write_bytes(b'unchanged executable')
        entries=[hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p.relative_to(self.package)) for p in [binary,checker]]
        (self.package/'SHA256SUMS').write_text('\n'.join(entries)+'\n')
        self.driver=module('aac_install',ROOT/'packaging/install.py');self.driver.HERE=self.package
    def action(self,command):
        with patch.object(sys,'argv',['install.py',command,'--root',str(self.root)]):self.driver.main()
    def test_install_and_undo_preserve_previous_plugin(self):
        self.action('install');self.assertEqual(self.target.read_bytes(),b'new plugin')
        self.action('uninstall');self.assertEqual(self.target.read_bytes(),b'previous plugin')
        self.assertEqual((self.root/'bin/resolve').read_bytes(),b'unchanged executable')
    def test_tampered_package_refused(self):
        (self.package/self.rel).write_bytes(b'corrupt')
        with self.assertRaisesRegex(RuntimeError,'checksum'):self.action('install')
        self.assertEqual(self.target.read_bytes(),b'previous plugin')
    def test_modified_installation_not_removed(self):
        self.action('install');self.target.write_bytes(b'user change')
        with self.assertRaisesRegex(RuntimeError,'changed'):self.action('uninstall')
        self.assertEqual(self.target.read_bytes(),b'user change')
    def test_running_resolve_refused(self):
        with patch.object(self.driver,'stopped',side_effect=RuntimeError('Close Resolve')):
            with self.assertRaisesRegex(RuntimeError,'Close Resolve'):self.action('install')
        self.assertEqual(self.target.read_bytes(),b'previous plugin')

FAKE='''#!PYTHON
import json,os,sys
from pathlib import Path
a=sys.argv[1:]
with open(os.environ['AAC_TEST_CALLS'],'a') as f:f.write(json.dumps(a)+'\\n')
if a[:2]==['image','inspect']:sys.exit(int(os.environ['AAC_TEST_NEW_IMAGE']))
if a[:2]==['start','--attach']:sys.exit(42)
sys.exit(0)
'''
class Builder(unittest.TestCase):
    def exercise(self,new_image):
        driver=module('aac_build',ROOT/'build.py')
        with tempfile.TemporaryDirectory(prefix='aac-build-test-') as td:
            base=Path(td);source=base/'source';source.mkdir();driver.HERE=source
            (source/'sources.lock.json').write_text(json.dumps({'image':'example@sha256:'+'0'*64}))
            sdk=base/'sdk'
            for rel in driver.SDK_FILES:
                p=sdk/rel;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('test')
            runtime=base/'podman';runtime.write_text(FAKE.replace('PYTHON',sys.executable,1));runtime.chmod(0o755)
            calls=base/'calls';out=base/'output'
            env=dict(os.environ,PATH=str(base)+os.pathsep+os.environ['PATH'],AAC_TEST_CALLS=str(calls),AAC_TEST_NEW_IMAGE=str(int(new_image)))
            with patch.dict(os.environ,env),patch.object(sys,'argv',['build.py','--sdk',str(sdk),'--output',str(out),'--runtime','podman']):
                with self.assertRaises(subprocess.CalledProcessError):driver.main()
            history=[json.loads(x) for x in calls.read_text().splitlines()]
            self.assertTrue(any(a[:2]==['rm','--force'] for a in history))
            self.assertEqual(any(a[:2]==['image','rm'] for a in history),new_image)
            self.assertFalse(out.exists());self.assertFalse(list(base.glob('.aac-build-*')))
    def test_failure_preserves_existing_image(self):self.exercise(False)
    def test_failure_cleans_new_image(self):self.exercise(True)

if __name__=='__main__':unittest.main()
