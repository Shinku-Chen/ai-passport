#!/usr/bin/env python3
"""Read-only RW5 source -> complete four-episode AI Passport graph/media packs.

python tools/nine_pack.py --source <9-nine-rw5> --episode kokoiro --out assets/nine/kokoiro
No source preprocessing scripts are executed. Source paths are only read.
"""
from __future__ import annotations
import argparse, array, hashlib, io, json, math, re, struct, subprocess, sys, zlib
from pathlib import Path
from PIL import Image, ImageOps

NONE=0xffffffff
EPISODES=('kokoiro','sorairo','haruiro','yukiiro')
COND={'==':1,'!=':2,'<':3,'<=':4,'>':5,'>=':6}
PERSIST={'kokoiro':{'loop_my','clear_true','titleBg'},
 'sorairo':{'loop1','clear_true','titleBg','loop_sr','cf_sr'},
 'haruiro':{'loop1','clear_true','titleBg','loop_hk','cf_hk'},
 'yukiiro':{'loop1','clear_true','titleBg','loop_my','loop_sr','cf_sr','loop_hk','cf_hk','loop_na','cf_na'}}

def js_graph(path):
 text=path.read_text('utf-8'); return json.loads(text.split('=',1)[1].strip().rstrip(';'))
def crc(data):return zlib.crc32(data)&NONE
def p32(*v):return struct.pack('<'+'I'*len(v),*(x&NONE for x in v))
def sha(data):return hashlib.sha256(data).hexdigest()

class Strings:
 def __init__(self):self.data=bytearray(b'\0');self.offsets={'':0};self.chars=set()
 def add(self,s):
  s=str(s or '');self.chars.update(s)
  if s not in self.offsets:self.offsets[s]=len(self.data);self.data.extend(s.encode('utf-8')+b'\0')
  return self.offsets[s]

class Media:
 def __init__(self,src,quality,op_fps):
  self.src=src;self.quality=quality;self.op_fps=op_fps;self.images=[];self.keys={};self.ops=[];self.audio=[];self.meta=[]
 def image(self,path,crop=None,sprite=False,size=None):
  if not path:return NONE
  key=(path,tuple(crop) if crop else None,sprite,tuple(size) if size else None)
  if key in self.keys:return self.keys[key]
  f=self.src/'src'/path.lstrip('/')
  if not f.is_file():raise ValueError(f'Missing image: {path}')
  with Image.open(f) as im:
   im=im.convert('RGBA' if sprite else 'RGB')
   if crop:im=im.crop(crop)
   if size is None:size=(max(1,round(im.width*320/432)),max(1,round(im.height*180/243))) if sprite else (320,180)
   if not sprite and abs(im.width/im.height-size[0]/size[1])>0.03:
    scaled=ImageOps.contain(im,size,Image.Resampling.LANCZOS);canvas=Image.new('RGB',size);canvas.paste(scaled,((size[0]-scaled.width)//2,(size[1]-scaled.height)//2));im=canvas
   else:im=im.resize(size,Image.Resampling.LANCZOS)
   if sprite:
    pal=im.quantize(colors=192,method=Image.Quantize.FASTOCTREE)
    rgba=pal.convert('RGBA'); colours=rgba.getcolors(maxcolors=256)
    if not colours:raise ValueError('Indexed image >256 colors')
    palette=sorted(colour for _,colour in colours);index={c:i for i,c in enumerate(palette)}
    raw=bytes(index[c] for c in rgba.getdata());blocks=[]
    for y in range(0,im.height,8):blocks.append(zlib.compress(raw[y*im.width:min(im.height,y+8)*im.width],9))
    record={'kind':1,'w':im.width,'h':im.height,'palette':b''.join(bytes(c) for c in palette),'blocks':blocks,'crc':crc(raw)}
   else:
    b=io.BytesIO();im.save(b,'JPEG',quality=self.quality,optimize=True,progressive=False,subsampling=2)
    data=b.getvalue();record={'kind':0,'w':im.width,'h':im.height,'data':data,'crc':crc(data)}
  record['bytes']=len(record['data']) if record['kind']==0 else len(record['palette'])+12+8*len(record['blocks'])+sum(map(len,record['blocks']))
  idx=len(self.images);self.keys[key]=idx;self.images.append(record);self.meta.append({'id':idx,'source':path,'crop':crop,'sprite':sprite,'size':list(size)})
  return idx
 def add_op(self):
  t=json.loads((self.src/'converted/opening/timeline.json').read_text('utf-8'));duration=t['durationMs'];frames=t['frames']
  # A lower output FPS is explicit; preserve source first/last and full duration.
  picked=[];seen=set()
  for i in range(math.ceil(duration*self.op_fps/1000)):
   index=min(len(frames)-1,round(i*t['fps']/self.op_fps))
   if index not in seen:picked.append(frames[index]);seen.add(index)
  if picked[-1] is not frames[-1]:picked.append(frames[-1])
  for i,f in enumerate(picked):
   end=picked[i+1]['atMs'] if i+1<len(picked) else duration
   self.ops.append((self.image(f['src']),int(f['atMs']),int(end-f['atMs'])))
  return {'sourceFrames':len(frames),'outputFrames':len(picked),'sourceFps':t['fps'],'targetFps':self.op_fps,'durationMs':duration}
 def add_audio(self,strings):
  try:
   import imageio_ffmpeg
   ffmpeg=imageio_ffmpeg.get_ffmpeg_exe()
  except ImportError:ffmpeg='ffmpeg'
  for name in ('title','opening'):
   source=self.src/f'src/common/{name}-bgm.opus'
   r=subprocess.run([ffmpeg,'-v','error','-i',str(source),'-f','s16le','-ac','1','-ar','16000','-'],capture_output=True,check=True)
   samples=array.array('h');samples.frombytes(r.stdout)
   if sys.byteorder!='little':samples.byteswap()
   data=ima_encode(samples);duration=round(len(samples)*1000/16000)
   self.audio.append((1,strings.add(name),data,duration,16000,1))
 def serialize(self,title,title_logo):
  image_off=128;op_off=image_off+32*len(self.images);audio_off=op_off+12*len(self.ops)
  blob=bytearray(audio_off+32*len(self.audio));records=[]
  for im in self.images:
   off=len(blob)
   if im['kind']==0:blob.extend(im['data'])
   else:
    blocks=im['blocks'];palette=im['palette'];blob.extend(p32(len(palette)//4,8,len(blocks)));blob.extend(palette)
    table=len(blob);blob.extend(b'\0'*(8*len(blocks)))
    for i,b in enumerate(blocks):
     struct.pack_into('<II',blob,table+i*8,len(blob),len(b));blob.extend(b)
   records.append((im['kind'],im['w'],im['h'],off,len(blob)-off,crc(blob[off:len(blob)]),0,0))
  for i,r in enumerate(records):struct.pack_into('<8I',blob,image_off+i*32,*r)
  for i,r in enumerate(self.ops):struct.pack_into('<3I',blob,op_off+i*12,*r)
  for i,(kind,name,data,dur,rate,ch) in enumerate(self.audio):
   off=len(blob);blob.extend(data);struct.pack_into('<8I',blob,audio_off+i*32,kind,name,off,len(data),dur,rate,ch,crc(data))
  fields=[2,len(blob),0,len(self.images),image_off,len(self.ops),op_off,len(self.audio),audio_off,*title,title_logo]
  blob[:8]=b'NINEIM1\0';struct.pack_into('<'+'I'*len(fields),blob,8,*fields);struct.pack_into('<I',blob,16,crc(blob))
  return bytes(blob)

IMA_STEPS=(7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767)
IMA_INDEX=(-1,-1,-1,-1,2,4,6,8)
def ima_encode(samples):
 result=bytearray();idx=0
 for pos in range(0,len(samples),256):
  chunk=list(samples[pos:pos+256]);chunk.extend([chunk[-1]]*(256-len(chunk)));pred=chunk[0];result.extend(struct.pack('<hBB',pred,idx,0));nibbles=[]
  for val in chunk[1:]:
   step=IMA_STEPS[idx];delta=val-pred;code=8 if delta<0 else 0;delta=abs(delta);diff=step>>3
   for bit,threshold in ((4,step),(2,step>>1),(1,step>>2)):
    if delta>=threshold:code|=bit;delta-=threshold;diff+=threshold
   pred=max(-32768,min(32767,pred+(-diff if code&8 else diff)));idx=max(0,min(88,idx+IMA_INDEX[code&7]));nibbles.append(code)
  nibbles.append(0)
  result.extend(nibbles[i]|nibbles[i+1]<<4 for i in range(0,256,2))
 return bytes(result)

def pack(source,episode,out,quality,op_fps):
 src=source/f'nine-{episode}-rw5';g=js_graph(src/'src/common/graph.js');s=Strings();media=Media(src,quality,op_fps)
 try:source_sha=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
 except subprocess.CalledProcessError:raise ValueError('Source must be a pinned Git checkout')
 title=json.loads((src/'src/manifest.json').read_text('utf-8'))['name'];ep_s=s.add(episode);title_s=s.add(title);sha_s=s.add(source_sha)
 nodes=list(g['nodes']);node_ids={n:i for i,n in enumerate(nodes)};var_names=[]
 original_engine=(src/'src/common/engine.js').read_text('utf-8')
 persistent=set(re.findall(r'(\w+)\s*:\s*true',original_engine.split('const PERSISTENT_KEYS =',1)[1].split('};',1)[0]))|{'loop1','true'}
 for end in g.get('endings',[]):persistent.add(end['id'])
 def var(k):
  if k not in var_names:var_names.append(k)
  if len(var_names)>128:raise ValueError('Too many state variables')
  return var_names.index(k)
 for k in sorted(persistent):var(k)
 conds=[];effects=[];edges=[];frames=[];visuals=[];layers=[];visual_map={};blocks=[];block=bytearray();text_raw=0;text_max=0
 def condition(w):
  if not w:return
  if w[0]=='all':
   for x in w[1:]:condition(x)
   conds.append((7,0,len(w)-1))
  else:conds.append((COND[w[1]],var(w[0]),int(w[2])))
 def effect(es):
  for k,op,val in es:effects.append((0 if op=='=' else 1,var(k),int(val)))
 def edge(e):
  cf=len(conds);condition(e.get('when'));ef=len(effects);effect(e.get('effects',[]))
  edges.append((node_ids[e['to']] if e['to']!='@end' else NONE,cf,len(conds)-cf,ef,len(effects)-ef,s.add(e.get('text',''))))
 def flush():
  if block:blocks.append(bytes(block));block.clear()
 def text(t):
  nonlocal text_raw,text_max
  b=t.encode('utf-8');s.chars.update(t);text_raw+=len(b);text_max=max(text_max,len(b))
  if len(b)+1>2048:raise ValueError('Single frame >2048 UTF-8 bytes')
  if len(block)+len(b)+1>8192:flush()
  r=(len(blocks),len(block),len(b));block.extend(b+b'\0');return r
 def visual(bg,sp):
  key=json.dumps([bg,sp],ensure_ascii=False,separators=(',',':'))
  if key in visual_map:return visual_map[key]
  first=len(layers)
  if bg:layers.append((media.image(bg),0,0,320,180,255))
  for a in sp:
   if isinstance(a,list):
    body,w,left=a[:3];opacity=a[3] if len(a)>3 and a[3] is not None else 1;patch=a[4] if len(a)>4 else None;h=a[13] if len(a)>13 and a[13] else 243;top=4 if h>=200 else 243-h
    d={'src':body,'width':w,'left':left,'height':h,'top':top,'opacity':opacity}
    if patch:
     for k,v in zip(('patch','patchWidth','patchHeight','patchLeft','patchTop','patchAtlasWidth','patchAtlasHeight','patchImageLeft','patchImageTop'),a[4:13]):d[k]=v
   else:d=a
   w=max(1,round(d['width']*320/432));h=max(1,round(d.get('height',243)*180/243));x=round(d['left']*320/432);y=round(d.get('top',4)*180/243);alpha=round(d.get('opacity',1)*255)
   layers.append((media.image(d['src'],sprite=True,size=(w,h)),x,y,w,h,alpha))
   if d.get('patch'):
    # CSS atlas is scaled to patchAtlasWidth/Height before negative image offset.
    with Image.open(src/'src'/d['patch'].lstrip('/')) as im:iw,ih=im.size
    ax=d['patchAtlasWidth'];ay=d['patchAtlasHeight'];px=-d['patchImageLeft'];py=-d['patchImageTop'];pw=d['patchWidth'];ph=d['patchHeight']
    crop=(round(px*iw/ax),round(py*ih/ay),round((px+pw)*iw/ax),round((py+ph)*ih/ay));ps=(max(1,round(pw*320/432)),max(1,round(ph*180/243)))
    layers.append((media.image(d['patch'],crop=crop,sprite=True,size=ps),round(d['patchLeft']*320/432),round(d['patchTop']*180/243),*ps,alpha))
  i=len(visuals);visuals.append((first,len(layers)-first));visual_map[key]=i;return i
 chapters=[(s.add(ch.get('title',ch.get('name',''))),s.add(ch.get('preview','')),node_ids[ch['node']]) for ch in g['chapters']]
 # Source node.chapter is a text label; stable chapter index maps by node/title.
 chapter_names={ch.get('title',ch.get('name','')):i for i,ch in enumerate(g['chapters'])}
 node_records=[]
 for nid in nodes:
  n=g['nodes'][nid];first=len(frames)
  for chunk in n.get('chunks',[]):
   rows=json.loads((src/'src/common/story'/chunk['file'].lstrip('/')).read_text('utf-8'));bg='';sp=[]
   for f in rows:
    if f.get('kind')=='op':frames.append((1,NONE,NONE,0,0,0));continue
    if 'art' in f:bg=f['art'].get('background','');sp=f['art'].get('sprites',[]);speaker=f.get('speaker','');tx=f.get('text','')
    else:bg=f.get('b',bg);sp=f.get('p',sp);speaker=f.get('s','');tx=f.get('t','')
    tb,to,tl=text(tx);frames.append((0,visual(bg,sp),tb,to,tl,s.add(speaker)))
  if len(frames)-first!=n['count']:raise ValueError(f'{nid}: source count mismatch')
  ef=len(effects);effect(n.get('effects',[]))
  for end in n.get('complete',[]):
   effect([(end,'=',1)])
   if end=='loop1' and episode=='kokoiro':effect([('loop_my','=',1)])
   if end=='true':effect([('clear_true','=',1)])
  ec=len(effects)-ef;nf=len(edges)
  for e in n.get('nexts',[]):edge(e)
  nc=len(edges)-nf;cf=len(edges)
  for e in n.get('choices',[]):edge(e)
  node_records.append((first,n['count'],ef,ec,nf,nc,cf,len(edges)-cf,s.add(n.get('title','')),chapter_names.get(n.get('chapter',''),NONE),s.add(nid),s.add(n.get('script','')+':'+n.get('label',''))))
 flush()
 endings=[(s.add(e['name']),var(e['id']),s.add(e.get('description',''))) for e in g.get('endings',[])]
 gallery=[];groups=json.loads((src/'converted/gallery.json').read_text('utf-8'))
 for i,group in enumerate(groups):
  for v in group['variants']:gallery.append((s.add(group['id']),media.image(v['src']),i))
 title_images=[media.image(f'/common/ui/title-bg{i}.jpg') for i in (1,2,3)]
 with Image.open(src/'src/common/ui/title-logo.png') as im:logo_size=(max(1,round(im.width*min(1,180/im.height,160/im.width))),max(1,round(im.height*min(1,180/im.height,160/im.width))))
 title_logo=media.image('/common/ui/title-logo.png',sprite=True,size=logo_size)
 op_report=media.add_op();media.add_audio(s)
 variables=[(s.add(k),1 if k in persistent else 0) for k in var_names]
 episode_id=EPISODES.index(episode)+1
 lookup={k:i for i,k in enumerate(var_names)}
 loop_key=lookup.get('loop1',NONE);clear_key=lookup.get('clear_true',NONE)
 blob=bytearray(256);header=[2,0,0,ep_s,title_s,sha_s,node_ids[g['start']]]
 for table,size in ((node_records,12),(frames,6),([(0,0,len(b),crc(b)) for b in blocks],4),(visuals,2),(layers,6),(variables,2),(conds,3),(effects,3),(edges,6),(chapters,3),(endings,3),(gallery,3)):
  header.extend((len(table),len(blob)))
  for r in table:blob.extend(p32(*r))
 header.extend((len(blob),len(s.data)))
 # reserved[0..3]: loop overlay key, clear key, episode id, gallery group count.
 header.extend((loop_key,clear_key,episode_id,len(groups)));header.extend([0]*25)
 blob.extend(s.data)
 block_table=header[12] # header: node count/off 7/8, frame 9/10, block11/12
 for i,b in enumerate(blocks):
  compressed=zlib.compress(b,9);struct.pack_into('<4I',blob,block_table+i*16,len(blob),len(compressed),len(b),crc(b));blob.extend(compressed)
 blob[:8]=b'NINESC2\0';header[1]=len(blob);struct.pack_into('<'+'I'*len(header),blob,8,*header);struct.pack_into('<I',blob,16,crc(blob))
 image_blob=media.serialize(title_images,title_logo);out.mkdir(parents=True,exist_ok=True)
 (out/'nine_script.bin').write_bytes(blob);(out/'nine_images.bin').write_bytes(image_blob)
 (out/'nine_symbols.txt').write_text(''.join(sorted(s.chars-set('\r\n\t'))),encoding='utf-8')
 by_top={}
 for m in media.meta:
  top=m['source'].split('/')[2] if m['source'].count('/')>2 else m['source']
  by_top[top]=by_top.get(top,0)+media.images[m['id']]['bytes']
 report={'episode':episode,'title':title,'sourceCommit':source_sha,'scriptBytes':len(blob),'imageMediaBytes':len(image_blob),'totalPackBytes':len(blob)+len(image_blob),'imageBytesByTop':by_top,'audioBytes':sum(len(a[2]) for a in media.audio),'nodes':len(nodes),'nodeOrder':nodes,'frames':len(frames),'textFrames':sum(f[0]==0 for f in frames),'textBytes':text_raw,'maxTextFrameBytes':text_max,'textBlocks':len(blocks),'maxTextBlock':max(map(len,blocks)),'variableCount':len(var_names),'variables':[{'name':k,'persistent':k in persistent} for k in var_names],'visuals':len(visuals),'layers':len(layers),'images':len(media.images),'galleryVariants':len(gallery),'galleryGroups':len(groups),'chapters':len(chapters),'endings':len(endings),'opening':op_report,'audio':[{'name':name,'durationMs':a[3],'bytes':len(a[2]),'rate':a[4]} for name,a in zip(('title','opening'),media.audio)],'jpegQuality':quality,'scriptSha256':sha(blob),'imageSha256':sha(image_blob),'maxImageBlockRaw':max((im['w']*min(8,im['h']) for im in media.images if im['kind']==1),default=0),'imageSources':media.meta}
 (out/'nine_pack_report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
 print(json.dumps({k:v for k,v in report.items() if k!='imageSources'},ensure_ascii=False))
 return report

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--episode',choices=EPISODES,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--jpeg-quality',type=int,default=38);p.add_argument('--op-fps',type=float,default=3)
 a=p.parse_args();pack(a.source,a.episode,a.out,a.jpeg_quality,a.op_fps)
if __name__=='__main__':main()
