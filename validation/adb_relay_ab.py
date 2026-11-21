"""Paired fixed-payload benchmark; no device or server configuration changes."""
import argparse
import csv
import hashlib
import random
import shlex
from adbutils import AdbClient
from adb_stages import receive

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output')
    p.add_argument('--scratch', required=True)
    p.add_argument('--stage', required=True)
    p.add_argument('--server', type=int, required=True)
    p.add_argument('--a', required=True)
    p.add_argument('--b', required=True)
    p.add_argument('--cycles', type=int, default=36)
    args = p.parse_args()
    client = AdbClient(host='127.0.0.1',port=args.server)
    devices = [client.device(x) for x in (args.a,args.b)]
    cd = 'cd '+shlex.quote(args.scratch)+' && '
    ref = devices[0].shell(cd+'cat last.png', encoding=None)
    assert ref.startswith(b'\x89PNG\r\n\x1a\n') and len(ref)>1048576
    digests = {k:hashlib.sha256(ref[:n]).digest() for k,n in
               [('replay',len(ref)),('one',1048576),('quarter',262144),('empty',0)]}
    rng = random.Random(1538)
    rows = []
    parity = {}
    for i in range(-2,args.cycles):
        order = list(digests)
        rng.shuffle(order)
        for k in order:
            if i%2==0: parity[k] = rng.randrange(2)
            first = parity[k] ^ (i%2)
            for pos,idx in enumerate((first,1-first)):
                b,d = receive(devices[idx],cd+shlex.quote(args.stage)+' '+k,False)
                if hashlib.sha256(b).digest()!=digests[k]:
                    raise ValueError('payload mismatch')
                if i>=0:rows.append(dict(cycle=i+1,path='AB'[idx],position=pos+1,mode=k,**d))
    keys=list(dict.fromkeys(k for r in rows for k in r))
    with open(args.output,'w',newline='',encoding='utf-8') as out:
        w=csv.DictWriter(out,keys,lineterminator='\n')
        w.writeheader()
        w.writerows(rows)

if __name__=='__main__':
    main()
