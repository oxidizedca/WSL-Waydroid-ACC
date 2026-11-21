"""Compare full PNG-to-RGB calls. Fixed-raw mode verifies RGBA, ancillary data and CRC."""
import argparse
import csv
import random
import shlex
import struct
import zlib
from adbutils import AdbClient
import cv2
import numpy as np
from adb_stages import receive

def metadata(data):
    off=8
    result=[]
    while off<len(data):
        n=int.from_bytes(data[off:off+4],'big')
        name=data[off+4:off+8]
        payload=data[off+8:off+8+n]
        crc=int.from_bytes(data[off+8+n:off+12+n],'big')
        if crc!=zlib.crc32(name+payload):raise ValueError('CRC')
        if name!=b'IDAT':result.append((name,payload))
        off+=12+n
    assert off==len(data)
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('output')
    p.add_argument('--variant',nargs=3,action='append',metavar=('NAME','ENDPOINT','ENCODER'),required=True)
    p.add_argument('--server',type=int,required=True)
    p.add_argument('--raw',action='append',help='fixed input file on Android; omit for live capture')
    p.add_argument('--cycles',type=int,default=24)
    a=p.parse_args()
    if len(a.variant)!=2:raise ValueError('requires a pair')
    client=AdbClient(host='127.0.0.1',port=a.server)
    devs=[client.device(v[1]) for v in a.variant]
    if a.raw:
        for raw in a.raw:
            ref=devs[0].shell('cat '+shlex.quote(raw),encoding=None)
            assert struct.unpack('<4I',ref[:16])==(1920,1080,1,1)
            rgba=np.frombuffer(ref[16:],np.uint8).reshape(1080,1920,4)
            meta=None
            for d,(_,_,exe) in zip(devs,a.variant):
                png=d.shell(shlex.quote(exe)+' < '+shlex.quote(raw),encoding=None)
                m=metadata(png)
                if meta is None:meta=m
                assert meta==m and np.array_equal(rgba,cv2.cvtColor(cv2.imdecode(np.frombuffer(png,np.uint8),
                    cv2.IMREAD_UNCHANGED),cv2.COLOR_BGRA2RGBA))
    rows=[]
    rng=random.Random(439)
    for i in range(-2,a.cycles):
        if i%2==0: flip=rng.randrange(2)
        j=flip^(i%2)
        for order,idx in enumerate((j,1-j)):
            label,_,exe=a.variant[idx]
            raw=a.raw[i%len(a.raw)] if a.raw else None
            cmd=(shlex.quote(exe)+' < '+shlex.quote(raw) if raw else
                 'screencap -p' if exe=='stock' else '/system/bin/screencap | '+shlex.quote(exe))
            data,ms=receive(devs[idx],cmd,True)
            if i>=0:rows.append(dict(cycle=i+1,position=order+1,variant=label,
                    source=raw.rsplit('/',1)[-1] if raw else 'live',**ms))
    with open(a.output,'w',encoding='utf-8',newline='') as f:
        w=csv.DictWriter(f,rows[0].keys(),lineterminator='\n')
        w.writeheader();w.writerows(rows)

if __name__=='__main__':
    main()
