"""Portable package integrity/failure tests; no host loading or GitHub writes."""
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import textwrap
import types
import unittest
from unittest.mock import patch
import zipfile

SPEC = importlib.util.spec_from_file_location('package', Path(__file__).resolve().parents[1]/'scripts/package.py')
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)


def pe(machine=0x8664):
    data=bytearray(256);data[:2]=b'MZ';struct.pack_into('<I',data,60,64)
    data[64:68]=b'PE\0\0';struct.pack_into('<HH',data,68,machine,1);struct.pack_into('<H',data,86,0x2000)
    return bytes(data)


def elf(machine=62):
    data=bytearray(64);data[:6]=b'\x7fELF\x02\x01';struct.pack_into('<HH',data,16,3,machine);return bytes(data)


def macho(cpu=0x0100000C):
    data=bytearray(32);data[:4]=b'\xcf\xfa\xed\xfe';struct.pack_into('<III',data,4,cpu,0,8);return bytes(data)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
        (self.root/'docs').mkdir();(self.root/'README.md').write_text('Public README\n')
        for name in package.DOCS:(self.root/'docs'/name).write_text('Public '+name+'\n')
        for name in ('PLAN.md','CHECKPOINTS.md','SUBAGENTS.md','private.sal','Analyzer.dll','SpdifTestSignal.dll'):
            (self.root/name).write_text('must never enter package')
        (self.root/'docs/PRIVATE.md').write_text('not allowlisted')
        self.binary=self.root/'SpdifAnalyzer.dll';self.binary.write_bytes(pe())
        self.source=dict(version='0.3.0',source_sha='a'*40,sdk_revision='b'*40,source_dirty=False,release_qualified=True,tag='v0.3.0')
        self.identity=patch.object(package,'source_identity',return_value=self.source);self.identity.start()

    def tearDown(self):self.identity.stop();self.tmp.cleanup()

    def make(self,name='out.zip'):
        return package.package(self.binary,self.root/name,root=self.root,release=True,tag='v0.3.0')

    def rewrite(self,path,change):
        with zipfile.ZipFile(path) as z:entries={n:z.read(n) for n in z.namelist()}
        change(entries);path.write_bytes(package.zip_bytes(entries))
        path.with_suffix('.zip.sha256').write_text(package.sha(path.read_bytes())+'  '+path.name+'\n')

    def test_binary_architectures(self):
        for binary,identity in ((pe(),('windows','x64')),(pe(0xaa64),('windows','arm64')),
                                (elf(),('linux','x64')),(elf(183),('linux','arm64')),
                                (macho(),('macos','arm64')),(macho(0x1000007),('macos','x64'))):
            self.assertEqual(package.binary_identity(binary),identity)
        fat=b'\xca\xfe\xba\xbe'+struct.pack('>I',2)+struct.pack('>IIIII',0x1000007,0,48,32,0)+struct.pack('>IIIII',0x100000c,0,80,32,0)+macho(0x1000007)+macho()
        self.assertEqual(package.binary_identity(fat),('macos','universal2'))
        for bad in (b'',b'x'*100,pe(0x14c),elf(3),macho(7),pe()[:70],fat[:100]):
            with self.assertRaises(ValueError):package.binary_identity(bad)

    def test_exact_allowlist_and_provenance(self):
        path=self.make();meta=package.verify(path,expected_source='a'*40,expected_tag='v0.3.0',require_release=True)
        self.assertEqual(meta['architecture'],'x64');self.assertEqual(meta['minimum_logic_version'],'2.3.43')
        with zipfile.ZipFile(path) as z:
            names=set(z.namelist());self.assertEqual(len(names),len(package.DOCS)+4)
            self.assertFalse(any(x in names for x in ('PLAN.md','private.sal','Analyzer.dll','SpdifTestSignal.dll','docs/PRIVATE.md')))
        with self.assertRaises(ValueError):package.verify(path,expected_source='c'*40)
        with self.assertRaises(ValueError):package.verify(path,expected_tag='v0.4.0')

    def test_deterministic_archive(self):
        self.assertEqual(self.make('one.zip').read_bytes(),self.make('two.zip').read_bytes())

    def test_all_platform_names(self):
        for name,data,platform in [('libSpdifAnalyzer.so',elf(),'linux'),('libSpdifAnalyzer.dylib',macho(),'macos')]:
            plugin=self.root/name;plugin.write_bytes(data)
            path=package.package(plugin,root=self.root)
            self.assertIn('-'+platform+'-',path.name);self.assertEqual(package.verify(path)['platform'],platform)

    def test_nonoverwrite_archive_and_checksum(self):
        path=self.make();original=path.read_bytes()
        with self.assertRaises(FileExistsError):self.make()
        self.assertEqual(original,path.read_bytes())
        checksum=self.root/'reserved.zip.sha256';checksum.write_text('do not replace')
        with self.assertRaises(FileExistsError):self.make('reserved.zip')
        self.assertEqual(checksum.read_text(),'do not replace');self.assertFalse((self.root/'reserved.zip').exists())

    def test_failed_promotion_cleans_own_files(self):
        original=package.os.link;calls=[]
        def link(source,dest):
            calls.append(dest)
            if len(calls)==2:raise OSError('injected promotion failure')
            return original(source,dest)
        with patch.object(package.os,'link',side_effect=link):
            with self.assertRaises(OSError):self.make()
        self.assertFalse((self.root/'out.zip').exists());self.assertFalse((self.root/'out.zip.sha256').exists())
        self.assertFalse(list(self.root.glob('.spdif-package-*')))

    def test_missing_document_has_no_output(self):
        (self.root/'docs/INSTALL.md').unlink()
        with self.assertRaises(FileNotFoundError):self.make()
        self.assertFalse((self.root/'out.zip').exists());self.assertFalse((self.root/'out.zip.sha256').exists())

    def test_refuses_qa_runtime_and_misnamed_binary(self):
        for name in ('SpdifTestSignal.dll','Analyzer.dll','other.dll'):
            binary=self.root/name;binary.write_bytes(pe())
            with self.assertRaises(ValueError):package.package(binary,self.root/'bad.zip',root=self.root)

    def test_external_and_internal_corruption(self):
        path=self.make();path.write_bytes(path.read_bytes()+b'changed')
        with self.assertRaises(ValueError):package.verify(path)
        path=self.make('internal.zip');self.rewrite(path,lambda e:e.update({'README.md':b'changed'}))
        with self.assertRaises(ValueError):package.verify(path)
        path=self.make('extra.zip');self.rewrite(path,lambda e:e.update({'../secret':b'no'}))
        with self.assertRaises(ValueError):package.verify(path)

    def test_architecture_claim_and_release_qualification(self):
        path=self.make()
        def corrupt(e):
            m=json.loads(e['PACKAGE.json']);m['architecture']='arm64';e['PACKAGE.json']=json.dumps(m).encode()
        self.rewrite(path,corrupt)
        with self.assertRaises(ValueError):package.verify(path)
        self.source['release_qualified']=False;path=package.package(self.binary,self.root/'local.zip',root=self.root)
        with self.assertRaises(ValueError):package.verify(path,require_release=True)
        def wrong_type(e):
            m=json.loads(e['PACKAGE.json']);m['release_qualified']='false';e['PACKAGE.json']=json.dumps(m).encode()
        self.rewrite(path,wrong_type)
        with self.assertRaises(ValueError):package.verify(path,require_release=True)

    def test_source_policy(self):
        self.identity.stop()
        (self.root/'src/core').mkdir(parents=True);(self.root/'src/core/Version.h').write_text('#define SPDIF_ANALYZER_VERSION "0.3.0"\n')
        (self.root/'CMakeLists.txt').write_text('GIT_REPOSITORY https://github.com/saleae/AnalyzerSDK.git GIT_TAG '+'b'*40)
        with patch.object(package,'git',side_effect=['a'*40,'','a'*40,'README.md']):
            self.assertTrue(package.source_identity(self.root,True,'v0.3.0','a'*40)['release_qualified'])
        for responses,tag,source in ((['a'*40,' M README.md'],'v0.3.0',None),(['a'*40,''],'v0.4.0',None),(['a'*40,'','c'*40],'v0.3.0',None),(['a'*40,''],'v0.3.0','c'*40)):
            with patch.object(package,'git',side_effect=responses):
                with self.assertRaises(ValueError):package.source_identity(self.root,True,tag,source)
        self.identity.start()

    def test_legacy_cli_and_readonly_verify(self):
        script=Path(package.__file__)
        output=self.root/'legacy.zip'
        run=subprocess.run([sys.executable,str(script),'--dll',str(self.binary),'--output',str(output)],capture_output=True,text=True)
        self.assertEqual(run.returncode,0,run.stderr)
        run=subprocess.run([sys.executable,str(script),'--verify',str(output)],capture_output=True,text=True)
        self.assertEqual(run.returncode,0,run.stderr)
        self.assertFalse(json.loads(run.stdout)['release_qualified'])
        before=output.read_bytes()
        run=subprocess.run([sys.executable,str(script),'--dll',str(self.binary),'--output',str(output)],capture_output=True,text=True)
        self.assertEqual(run.returncode,2);self.assertEqual(before,output.read_bytes())

    def test_actual_publish_script_failure_and_success_paths(self):
        # Execute the exact inline publication script with an in-memory HTTP
        # server substitute. No network or actual repository/release writes.
        workflow=(Path(package.__file__).parents[1]/'.github/workflows/release.yml').read_text()
        body=workflow.split('Stage a new draft, verify uploaded bytes, then publish',1)[1]
        body=body.split("python - <<'PY'\n",1)[1].split('\n          PY',1)[0]
        code=compile(textwrap.dedent(body),'release-publish-inline','exec')
        directory=self.root/'validated';directory.mkdir()
        for platform in ('windows','linux','macos'):
            path=directory/f'SpdifAnalyzer-0.3.0-{platform}-x64.zip';path.write_bytes(platform.encode())
            path.with_suffix('.zip.sha256').write_text(package.sha(path.read_bytes())+'  '+path.name+'\n')
        (directory/'RELEASE_NOTES.md').write_text('Reviewed source-specific notes')
        (directory/'RELEASE_MANIFEST.json').write_text(json.dumps(dict(source_sha='a'*40,version='0.3.0')))
        for existing,corrupt in ((True,False),(False,True),(False,False)):
            requests=[];assets={};published=[]
            def response(value):return io.BytesIO(value if isinstance(value,bytes) else json.dumps(value).encode())
            def network(request,timeout):
                from urllib.parse import urlsplit,parse_qs
                url=request.full_url;method=request.get_method();requests.append((method,url))
                if '/releases?per_page=' in url:return response([{'tag_name':'v0.3.0','draft':True}] if existing else [])
                if method=='POST' and url.endswith('/releases'):
                    self.assertTrue(json.loads(request.data)['draft'])
                    return response({'id':17,'draft':True,'upload_url':'https://uploads.example/assets{?name,label}'})
                if method=='POST' and url.startswith('https://uploads.example/'):
                    name=parse_qs(urlsplit(url).query)['name'][0];index=len(assets)+1;assets[index]=(name,request.data)
                    return response({'id':index,'state':'uploaded','size':len(request.data),'name':name,'url':'https://api.example/assets/'+str(index)})
                if url.startswith('https://api.example/assets/'):
                    index=int(url.rsplit('/',1)[1]);return response(assets[index][1]+(b'corrupt' if corrupt else b''))
                if method=='GET' and url.endswith('/17/assets'):return response([{'id':i} for i in assets])
                if method=='PATCH' and url.endswith('/releases/17'):
                    published.append(json.loads(request.data));return response({'draft':False,'html_url':'https://example/release'})
                self.fail('Unexpected release request '+method+' '+url)
            previous=Path.cwd()
            try:
                os.chdir(self.root)
                with patch.dict(os.environ,{'GH_TOKEN':'test-only','REPOSITORY':'owner/repo','TAG':'v0.3.0','SOURCE_SHA':'a'*40}),patch('urllib.request.build_opener',return_value=types.SimpleNamespace(open=network)),patch('builtins.print'):
                    if existing or corrupt:
                        with self.assertRaises(SystemExit):exec(code,{})
                    else:exec(code,{})
            finally:os.chdir(previous)
            self.assertEqual(len(published),0 if existing or corrupt else 1)
            if existing:self.assertTrue(all(method=='GET' for method,_ in requests))
            if corrupt:self.assertTrue(any(method=='POST' and url.endswith('/releases') for method,url in requests))

    def publisher_helpers(self):
        workflow=(Path(package.__file__).parents[1]/'.github/workflows/release.yml').read_text()
        body=workflow.split('Stage a new draft, verify uploaded bytes, then publish',1)[1]
        body=body.split("python - <<'PY'\n",1)[1].split('\n          PY',1)[0]
        helper=textwrap.dedent(body).split('# Authenticated paginated listing',1)[0]
        namespace={}
        with patch.dict(os.environ,{'GH_TOKEN':'sentinel-must-not-forward','REPOSITORY':'owner/repo','TAG':'v0.3.0'}):
            exec(compile(helper,'release-http-helpers','exec'),namespace)
        return namespace

    def test_redirect_authorization_origin_policy(self):
        # Directly exercise the actual configured handler; never open a socket.
        from urllib.request import Request
        helpers=self.publisher_helpers();handler=helpers['ReleaseRedirectHandler']()
        original=Request('https://api.github.com/releases/assets/1',headers={'Authorization':'Bearer sentinel-must-not-forward','Accept':'application/octet-stream'})
        for target in ('https://release-assets.githubusercontent.com/asset?sig=signed',
                       'https://api.github.com:444/asset','https://other.example/asset'):
            redirected=handler.redirect_request(original,None,302,'Found',{},target)
            self.assertIsNone(redirected.get_header('Authorization'))
            self.assertNotIn('sentinel-must-not-forward',repr(redirected.header_items()))
            again=handler.redirect_request(redirected,None,302,'Found',{},target+'/next')
            self.assertIsNone(again.get_header('Authorization'))
        for target in ('https://api.github.com/next','https://API.GITHUB.COM:443/next','/relative'):
            redirected=handler.redirect_request(original,None,302,'Found',{},target)
            self.assertEqual(redirected.get_header('Authorization'),'Bearer sentinel-must-not-forward')
        for target in ('http://api.github.com/next','http://release-assets.githubusercontent.com/asset',
                       'file:///tmp/asset','https://user:secret@api.github.com/asset','https://api.github.com:0/asset'):
            with self.assertRaises(ValueError):handler.redirect_request(original,None,302,'Found',{},target)

    def test_remote_json_has_a_read_bound(self):
        helpers=self.publisher_helpers()
        self.assertEqual(helpers['read_json'](io.BytesIO(b'{"draft":true}')),{'draft':True})
        with self.assertRaises(SystemExit):helpers['read_json'](io.BytesIO(b' '*(16*1024*1024+1)))


if __name__=='__main__':unittest.main()
