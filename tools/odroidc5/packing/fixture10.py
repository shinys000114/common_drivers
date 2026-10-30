#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
"""Offline fixture and explicit 32-bit RGB10 packing hypotheses; no device access."""
from pathlib import Path
import argparse,collections,hashlib,json,struct
FORMATS={'XRGB2101010':(20,10,0),'XBGR2101010':(0,10,20),
         'RGBX1010102':(22,12,2),'BGRX1010102':(2,12,22)}
def rgb(x,y,pattern="matrix"):
 v=x%1024
 if y<128:return v,64,512
 if y<256:return 64,v,512
 if y<384:return 64,512,v
 if y<512:return v,v,v
 if pattern=="matrix" and y>=640:return (1023 if x&1 else 0,1023 if y&1 else 0,1023 if x&32 else 0)
 return (x+3*y)%1024,(3*x+5*y)%1024,(7*x+11*y)%1024
def pack(fmt,c):return sum(v<<s for v,s in zip(c,FORMATS[fmt]))
def unpack(fmt,w):return tuple((w>>s)&1023 for s in FORMATS[fmt])
def selftest():
 checks=0
 for name in FORMATS:
  for c in [(0,0,0),(1023,1023,1023),(1,2,3),(64,65,66),(256,257,258),(511,512,513),(1020,1021,1022),(17,333,719)]:
   w=pack(name,c);assert unpack(name,w)==c
   assert struct.unpack('<I',struct.pack('<I',w))[0]==w;checks+=2
 for ch,y in [(0,0),(1,128),(2,256)]:
  vals=[rgb(x,y)[ch] for x in range(1024)];assert vals==list(range(1024))
  assert set(v&3 for v in vals)=={0,1,2,3}
  assert len(set(v&~3 for v in vals))==256;checks+=3
 assert rgb(0,640)==(0,0,0) and rgb(33,641)==(1023,1023,1023);checks+=2
 assert rgb(1,640,"legacy")==(897,131,903);checks+=1
 return checks

def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--selftest',action='store_true');ap.add_argument('--pattern',choices=['matrix','legacy'],default='matrix');ap.add_argument('--make-source',type=Path);ap.add_argument('--analyze',type=Path);ap.add_argument('--output',type=Path);ap.add_argument('--width',type=int,default=1920);ap.add_argument('--height',type=int,default=720);ap.add_argument('--pitch',type=int);args=ap.parse_args()
 if args.selftest:print(json.dumps({'synthetic_checks':selftest(),'device_access':False,'precision_qualified':False}));return
 assert args.width>=1024 and args.height>=512
 pitch=args.pitch or ((args.width*4+63)//64*64);assert pitch>=args.width*4 and pitch%64==0
 if args.make_source:
  out=args.make_source;out.mkdir(parents=True,exist_ok=False);data=bytearray(pitch*args.height)
  for y in range(args.height):
   for x in range(args.width):struct.pack_into('<I',data,y*pitch+x*4,pack('XBGR2101010',rgb(x,y,args.pattern)))
  (out/'source-XBGR2101010.raw').write_bytes(data)
  meta={'format':'DRM_FORMAT_XBGR2101010','fixture_pattern':args.pattern,'width':args.width,'height':args.height,'pitch':pitch,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'low_bits_source':True,'actual_precision_qualified':False,'requires':'LINEAR primary; opaque; other planes off; NULL CTM/gamma; preserved source until WB fence'}
  (out/'SOURCE.json').write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps(meta));return
 assert args.analyze and args.output
 data=args.analyze.read_bytes();assert len(data)==pitch*args.height
 report={'raw_sha256':hashlib.sha256(data).hexdigest(),'fixture_pattern':args.pattern,'width':args.width,'height':args.height,'pitch':pitch,'capture_completed':None,'actual_precision_qualified':False,'interpretation':'Explicit hypotheses only; root must verify source, mode, tap and retired target. No best fit is selected.','hypotheses':[]}
 for fmt,shifts in FORMATS.items():
  errors=[0,0,0];exact=[0,0,0];maxerr=[0,0,0];unused=collections.Counter();low=[collections.Counter() for _ in range(3)];unique=[set() for _ in range(3)]
  mask=0xffffffff ^ sum(1023<<s for s in shifts)
  for y in range(args.height):
   for x in range(args.width):
    w=struct.unpack_from('<I',data,y*pitch+x*4)[0];observed=unpack(fmt,w);expected=rgb(x,y,args.pattern);unused[w&mask]+=1
    for ch in range(3):
     delta=abs(observed[ch]-expected[ch]);errors[ch]+=delta;maxerr[ch]=max(maxerr[ch],delta);exact[ch]+=delta==0
     if y==ch*128 and x<1024:low[ch][observed[ch]&3]+=1;unique[ch].add(observed[ch])
  n=args.width*args.height
  report['hypotheses'].append({'format':fmt,'exact_rgb_payload_match':exact==[n,n,n],'component_mae':[v/n for v in errors],'component_max_error':maxerr,'component_exact_fraction':[v/n for v in exact],'isolated_ramp_distinct_codes':[len(v) for v in unique],'isolated_ramp_lowbits':[dict(v) for v in low],'unused_word_bits':dict(unused)})
 args.output.parent.mkdir(parents=True,exist_ok=True);assert not args.output.exists();args.output.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'report':str(args.output),'hypotheses':len(FORMATS),'actual_precision_qualified':False}))
if __name__=='__main__':main()
