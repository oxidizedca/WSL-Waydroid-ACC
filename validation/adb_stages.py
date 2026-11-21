"""Attribute adbutils screenshot latency without storing game frames on Windows."""
import argparse
import csv
from hashlib import sha256
from itertools import permutations
from pathlib import Path
from time import perf_counter_ns as now
import shlex

from adbutils import adb
import cv2
import numpy as np

def receive(dev, cmd, decode):
    start = now()
    c = dev.open_shell(cmd)
    try:
        opened = now()
        c.conn.settimeout(15)
        read = c.read
        first = None
        def first_read(n):
            nonlocal first
            b = read(n)
            if b:
                first = now()
            c.read = read
            return b
        c.read = first_read
        data = c.read_until_close(encoding=None)
        eof_seen = now()
    finally:
        c.close()
    end = now()
    d = dict(nbytes=len(data), open_ms=(opened-start)/1e6, receive_ms=(end-start)/1e6)
    d.update(close_ms=(end-eof_seen)/1e6)
    if first is not None:
        d.update(first_ms=(first-start)/1e6, body_ms=(end-first)/1e6, payload_eof_ms=(eof_seen-first)/1e6)
    if decode:
        bgr = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_COLOR)
        converted = now()
        rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
        done = now()
        assert rgb.shape == (1080,1920,3)
        assert data.startswith(b'\x89PNG\r\n\x1a\n') and data.endswith(b'IEND\xaeB\x60\x82')
        d.update(imdecode_ms=(converted-end)/1e6, rgb_ms=(done-converted)/1e6, total_ms=(done-start)/1e6)
    return data,d

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output')
    p.add_argument('--bin', required=True)
    p.add_argument('--scratch', required=True)
    p.add_argument('--device', default='127.0.0.1:15555')
    p.add_argument('--cycles', type=int, default=24)
    a = p.parse_args()
    dev = adb.device(a.device)
    stage = shlex.quote(a.bin+'/stage')
    probe = shlex.quote(a.bin+'/probe')
    cd = 'cd '+shlex.quote(a.scratch)+' && '
    orders = list(permutations(('normal','pipe','block')))
    rows = []
    for i in range(-2, a.cycles):
        batch = {}
        for group in orders[i%6]:
            controls = ['replay','quarter','one','empty']
            j = i % 4
            seq = ['split']+controls[j:]+controls[:j] if group=='block' else [group]
            for k in seq:
                cmd = ('screencap -p' if k=='normal' else cd +
                    (f'({stage} capture | {probe})' if k=='pipe' else f'{stage} {k}'))
                data,d = receive(dev,cmd,k in ('normal','pipe','split'))
                if k=='split':
                    expected = {key:sha256(data[:n]).digest() for key,n in
                                [('replay',len(data)),('quarter',262144),('one',1048576),('empty',0)]}
                elif k in controls:
                    assert sha256(data).digest()==expected[k],k
                batch[k] = dict(cycle=i+1,variant=k,**d)
        t = list(map(int, dev.shell(cd+'cat begin pipe-times split-times').split()))
        start,_,_,header,eof,ready,sent,sa,sb,sc = t
        assert start<=header<=eof<=ready<=sent and sa<=sb<=sc
        for k,x,y,z in [('pipe',start,eof,ready),('split',sa,sb,sc)]:
            r = batch[k]
            r.update(capture_ms=(y-x)/1e6,encode_ms=(z-y)/1e6,
                     request_first_ms=r['first_ms']-(z-x)/1e6,
                     adb_envelope_ms=r['receive_ms']-(z-x)/1e6)
        batch['pipe'].update(header_ms=(header-start)/1e6,raw_tail_ms=(eof-header)/1e6,
                             write_ms=(sent-ready)/1e6)
        if i>=0:
            rows.extend(batch.values())
    keys = list(dict.fromkeys(k for r in rows for k in r))
    with Path(a.output).open('w',encoding='utf-8',newline='') as f:
        w = csv.DictWriter(f,keys,lineterminator='\n')
        w.writeheader()
        for r in rows:
            w.writerow({k:round(v,4) if isinstance(v,float) else v for k,v in r.items()})
    print(f'{len(rows)} rows; cv2 {cv2.__version__}')

if __name__=='__main__':
    main()

