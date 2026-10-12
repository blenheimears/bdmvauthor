#!/usr/bin/env python3
"""Validate CLPI sequence PCR PID against PMT and actual PCR packets.

Usage: python3 scripts/check_clpi_pcr.py /path/to/BDMV-or-parent-or-disc.iso
The .iso reader requires the optional libudfread shared library on Linux.
"""
from __future__ import annotations
import ctypes as C
import sys
from pathlib import Path


def u16(b): return int.from_bytes(b,'big')
def u32(b): return int.from_bytes(b,'big')

class Files:
    def __init__(self, path):
        self.path=Path(path); self.lib=None; self.ctx=None
        if self.path.is_file():
            self.lib=C.CDLL('libudfread.so.0')
            l=self.lib
            for name,rest,args in (
                ('udfread_init',C.c_void_p,()),
                ('udfread_open',C.c_int,(C.c_void_p,C.c_char_p)),
                ('udfread_close',None,(C.c_void_p,)),
                ('udfread_file_open',C.c_void_p,(C.c_void_p,C.c_char_p)),
                ('udfread_file_close',None,(C.c_void_p,)),
                ('udfread_file_read',C.c_ssize_t,(C.c_void_p,C.c_void_p,C.c_size_t)),
                ('udfread_file_size',C.c_int64,(C.c_void_p,)),
            ):
                fn=getattr(l,name);fn.restype=rest;fn.argtypes=args
            self.ctx=l.udfread_init()
            if not self.ctx or l.udfread_open(self.ctx,str(self.path).encode())!=0:
                raise RuntimeError(f'cannot open disc image {self.path} with libudfread')
        else:
            if (self.path/'BDMV').is_dir(): self.path /= 'BDMV'
            if not (self.path/'CLIPINF').is_dir(): raise ValueError('expected BDMV folder, parent or ISO')
    def read(self,part,limit=None):
        if not self.lib:
            with (self.path/part).open('rb') as f: return f.read(limit)
        fn=b'/BDMV/'+str(part).encode().lstrip(b'/')
        h=self.lib.udfread_file_open(self.ctx,fn)
        if not h: raise FileNotFoundError(fn)
        try:
            sz=self.lib.udfread_file_size(h)
            count=sz if limit is None else min(sz,limit)
            chunks=[]
            while count>0:
                b=C.create_string_buffer(min(count,1024*1024))
                n=self.lib.udfread_file_read(h,b,len(b))
                if n<=0: break
                chunks.append(b.raw[:n]);count-=n
            return b''.join(chunks)
        finally: self.lib.udfread_file_close(h)
    def close(self):
        if self.lib and self.ctx: self.lib.udfread_close(self.ctx)

def analyze(stream:bytes):
    if len(stream)%192: raise ValueError('not 192-byte M2TS source packets')
    pmtpid=None; pcrpid=None; physical=set(); pid_set=set()
    for off in range(0,len(stream),192):
        p=stream[off+4:off+192]
        if p[0]!=0x47: raise ValueError(f'bad TS sync at source packet {off//192}')
        pid=((p[1]&31)<<8)|p[2]
        pid_set.add(pid)
        afc=(p[3]>>4)&3
        if afc in (2,3) and p[4]>=7 and p[5]&0x10:
            physical.add(pid)
        pos=4
        if afc in (2,3):pos+=1+p[4]
        if pos>=188 or afc not in (1,3) or not p[1]&0x40:continue
        if pid not in (0,pmtpid):continue
        pointer=p[pos];pos+=1+pointer
        if pos+13>=188:continue
        if pid==0 and p[pos]==0:
            length=u16(p[pos+1:pos+3])&0xfff
            if length<13: continue
            end=pos+3+length-4  # skip CRC32
            for entry in range(pos+8,end,4):
                if entry+4>188:break
                program=u16(p[entry:entry+2])
                if program: pmtpid=u16(p[entry+2:entry+4])&0x1fff
        elif pid==pmtpid and p[pos]==2:
            pcrpid=u16(p[pos+8:pos+10])&0x1fff
    return pmtpid,pcrpid,physical,pid_set

def check(f,idx):
    clip=f.read(f'CLIPINF/{idx}.clpi')
    if clip[:4]!=b'HDMV':raise ValueError('invalid CLPI header')
    off=u32(clip[8:12]); assert off+22<=len(clip)
    if clip[off+5]!=1 or clip[off+10]!=1:
        raise ValueError('non-single-sequence CLPI: inspect manually')
    declared=u16(clip[off+12:off+14])
    start=u32(clip[off+18:off+22]);end=u32(clip[off+22:off+26]);
    m2ts=f.read(f'STREAM/{idx}.m2ts', 3*1024*1024)
    m2ts=m2ts[:len(m2ts)//192*192]
    pmt,pcr,physical,pids=analyze(m2ts)
    good=(declared==pcr and declared in physical)
    print(f'{idx}: CLPI=0x{declared:04x} PMT_PCR=0x{(pcr or 0):04x} realPCR={sorted(hex(x) for x in physical)} '
          f'window={start/45000:.3f}-{end/45000:.3f}s '+('OK' if good else 'FAIL'))
    if not good:raise ValueError(f'{idx} CLPI PCR PID does not correspond to the transport PCR')

if __name__=='__main__':
    if len(sys.argv)!=2:
        print(__doc__,file=sys.stderr);sys.exit(2)
    f=Files(sys.argv[1]);bad=0
    try:
        for idx in ('00000','00001','00002'):
            try:check(f,idx)
            except Exception as exc:
                bad+=1;print(f'{idx}: ERROR: {exc}',file=sys.stderr)
    finally:f.close()
    sys.exit(1 if bad else 0)
