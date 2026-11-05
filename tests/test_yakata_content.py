"""Differential story tests against the pinned RW5 data, plus asset contracts."""
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from yakata_import import load

class ContentTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat,cls.ops=load(ROOT/'assets/yakata-source')
        cls.content=(ROOT/'main/gal/gal_content.c').read_text('utf-8')
        cls.events=[(k,int(i)) for k,i in re.findall(r'\{(GAL_EV_\w+),(\d+),GAL_UNLOCK_NONE\}',cls.content)]
        cls.report=json.loads((ROOT/'main/yakata_data/report.json').read_text('utf-8'))
        cls.starts={}; cursor=0
        for scene in cls.cat['scenes']:
            cls.starts[scene['id']]=cursor
            cursor+=int(scene['id']==scene['chapterKey'])+len(cls.ops[scene['id']])+1
        assert cursor==len(cls.events)

    def test_all_text_and_events(self):
        lines=[(json.loads(n),json.loads(t)) for n,t in re.findall(r'\{("(?:[^"\\]|\\.)*"),("(?:[^"\\]|\\.)*"),GAL_UNLOCK_NONE,&img_\d+,0\}',self.content)]
        expected=[(o.get('speaker',''),o['text']) for s in self.cat['scenes'] for o in self.ops[s['id']] if o['type']=='text']
        self.assertEqual(lines,expected)
        self.assertEqual(len(lines),17409)
        self.assertEqual(sum(k=='GAL_EV_CHOICE' for k,i in self.events),11)
        self.assertEqual(sum(k=='GAL_EV_TRANSITION' for k,i in self.events),99)
        for k,i in self.events:
            self.assertLess(i, {'GAL_EV_LINE':len(lines),'GAL_EV_CHOICE':11,'GAL_EV_TRANSITION':99,'GAL_EV_ENDING':21,'GAL_EV_FLOW':198}[k])

    def test_jpeg_and_gallery_contract(self):
        blob=(ROOT/'main/yakata_data/yakata_art.bin').read_bytes()
        entries=re.findall(r'\.w=(\d+), \.h=(\d+) \}, \.data_size=(\d+), \.data=yakata_art_start\+(\d+)',self.content)
        self.assertEqual(len(entries),self.report['jpeg_images'])
        for w,h,n,off in entries:
            w,h,n,off=map(int,(w,h,n,off))
            self.assertLessEqual(off+n,len(blob)); self.assertEqual(off%4,0)
            self.assertTrue(blob[off:off+n].startswith(b'\xff\xd8\xff\xe0'))
            self.assertEqual(blob[off+n-2:off+n],b'\xff\xd9')
            self.assertIn((w,h), {(320,240),(64,40)})
        offsets=list(map(int,re.search(r'yakata_cg_offsets\[\]=\{([^}]+)',self.content).group(1).split(',')))
        self.assertEqual(len(offsets),57)
        self.assertEqual(offsets[-1],311)
        self.assertEqual([b-a for a,b in zip(offsets,offsets[1:])],[len(g['variants']) for g in self.cat['gallery']])

    def test_compiled_routes_match_source(self):
        names=self.report['variables']; key_index={k:i for i,k in enumerate(names)}
        def effects(v,items):
            v=list(v)
            for x in items or []:
                i=key_index[x['key']]
                v[i]=v[i]+x['add'] if 'add' in x else int(x['value'])
            return v
        def condition(c,v):
            if not c:return True
            if 'all' in c:return all(condition(x,v) for x in c['all'])
            value=v[key_index[c['key']]]
            return value==c['equals'] if 'equals' in c else bool(value)
        inputs=[];expected=[]
        # Includes zero/default, every flag combination, and non-unit counters.
        states=[list(x)+[0]*(8-len(names)) for x in itertools.product((0,1,2),repeat=len(names))]
        choices=[o for s in self.cat['scenes'] for o in self.ops[s['id']] if o['type']=='choice']
        for v in states:
            for i,s in enumerate(self.cat['scenes']):
                w=effects(v,s.get('onExit'))
                target=s.get('next')
                if s.get('routes'):
                    target=next(r['target'] for r in s['routes'] if condition(r['when'],w))
                inputs.append([0,i,0]+v);expected.append([self.starts.get(target,-1)]+w)
            for i,o in enumerate(choices):
                for j,it in enumerate(o['items']):
                    inputs.append([1,i,j]+v);expected.append([self.starts[it['scene']]]+effects(v,it.get('changes')))
        with tempfile.TemporaryDirectory(prefix='yakata-flow-') as d:
            exe=Path(d)/('oracle.exe' if os.name=='nt' else 'oracle')
            cc=shlex.split(os.environ.get('CC','cc'))
            if os.name=='nt' and os.environ.get('MSYSTEM'):
                cc=['bash','-c','exec "$@"','--']+cc
            subprocess.run(cc+['-std=c11','-Wall','-Wextra','-Werror','-Imain/gal','tests/yakata_flow_oracle.c','main/gal/yakata_flow.c','-o',str(exe)],cwd=ROOT,check=True)
            run=subprocess.run([str(exe)],input=''.join(' '.join(map(str,x))+'\n' for x in inputs),text=True,capture_output=True,check=True)
            actual=[list(map(int,x.split())) for x in run.stdout.splitlines()]
            self.assertEqual(actual,expected)
        print(f'Compiled C/source differential: {len(inputs)} state transitions PASS')

    def test_pinned_source_manifest(self):
        source=ROOT/'assets/yakata-source'
        manifest=json.loads((source/'MANIFEST.json').read_text('utf-8'))
        self.assertEqual(manifest['commit'],self.report['source_commit'])
        for name,digest in manifest['files'].items():
            self.assertEqual(hashlib.sha256((source/name).read_bytes()).hexdigest(),digest,name)

    def test_application_runtime(self):
        header=(ROOT/'main/gal/gal_ui.h').read_text('utf-8')
        ui=['#include "gal_ui.h"', '#include "gal_text.h"']
        for result,name,args in re.findall(r'(bool|void|int)\s+(gal_ui_\w+)\(([^;]+)\);',header):
            body='return true;' if result=='bool' else ''
            if name=='gal_ui_typing':body='return ui->typing_active;'
            if name=='gal_ui_show_full_text':body='ui->typing_active=false;'
            if name=='gal_ui_set_text':body='ui->typing_active=ms_per_char!=0 && page_text && page_text[0]!=0;'
            if name=='gal_ui_text_pages':body='return gal_text_pages(text,34,GAL_TEXT_LINES_SMALL,offsets,max_offsets);'
            ui.append(f'{result} {name}({args}) {{ {body} }}')
        with tempfile.TemporaryDirectory(prefix='yakata-runtime-') as d:
            stub=Path(d)/'display.c';stub.write_text('\n'.join(ui),encoding='utf-8')
            exe=Path(d)/('runtime.exe' if os.name=='nt' else 'runtime')
            cc=shlex.split(os.environ.get('CC','cc'))
            if os.name=='nt' and os.environ.get('MSYSTEM'):
                cc=['bash','-c','exec "$@"','--']+cc
            sources=['tests/test_yakata_runtime.c']+['main/gal/'+n+'.c' for n in ('gal_content','yakata_flow','gal_text','gal_save','gal_progress','gal_unlock','gal_settings')]
            subprocess.run(cc+['-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Itests/yakata_stubs','-Imain/gal']+sources+[str(stub),'-o',str(exe)],cwd=ROOT,check=True)
            subprocess.run([str(exe)],check=True)

if __name__=='__main__':unittest.main()
