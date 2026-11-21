from pathlib import Path
import hashlib,struct,re,subprocess as sp
from collections import defaultdict
def measure(elf):
    import os
    assert os.sysconf('SC_PAGE_SIZE')==4096
    assert hashlib.sha256(elf.read_bytes()).hexdigest() in {
        '52da392d9b6bed1c28a1ce9cd4fe2426adbd6f61e007dbe90181b734e7c679ba',
        '2e43e41280440386044eb59618069f4b614d4499c9f57eaa91c8d9f20f603b2c',
    }, 'unsupported measurement ABI'
    names=('libcrypto.so.3','libstdc++.so.6')
    syms={w[3]:(int(w[0],16),int(w[1],16)) for line in sp.check_output(['nm','-S',elf],text=True).splitlines() if len(w:=line.split())==4}
    verified=set(); pfns=defaultdict(set); rows=[]
    for p in Path('/proc').glob('[0-9]*'):
        try:
            if 'lxc.payload.waydroid' not in (p/'cgroup').read_text():continue
            maps=(p/'maps').read_text();m=re.search(r'^([0-9a-f]+)-\w+ r--p 00000000 \S+ (\d+) +(/system/lib64/libEGL_angle.so)$',maps,re.M)
            if not m:continue
            base=int(m[1],16);root=p/'root'; installed=root/m[3][1:];st=installed.stat();assert st.st_ino==int(m[2])
            if (k:=(st.st_dev,st.st_ino)) not in verified:
                assert installed.read_bytes()==elf.read_bytes();verified.add(k)
            proc=(p/'cmdline').read_bytes().split(b'\0')[0].decode().split('/')[-1]
            with (p/'mem').open('rb',buffering=0) as mem,(p/'pagemap').open('rb',buffering=0) as pm:
                def meta(k):
                    off,n=syms[k];mem.seek(base+off);return mem.read(n)
                tab=meta('g_libs');assert len(tab)==16*264
                n,=struct.unpack('<i',meta('g_nlibs'));assert 0<=n<=16
                for i in range(n):
                    ent=tab[i*264:(i+1)*264];addr,va0,sz=struct.unpack_from('<3Q',ent)
                    path=Path(ent[136:].split(b'\0')[0].decode());name=path.name
                    if name not in names:continue
                    with (root/str(path)[1:]).open('rb') as lib:
                        eh=lib.read(64);assert eh[:4]==b'\x7fELF';stride,num=struct.unpack_from('<HH',eh,54);lib.seek(struct.unpack_from('<Q',eh,32)[0]);ph=lib.read(stride*num)
                    present=file=exclusive=0
                    for j in range(num):
                        typ,fl,off,va,_,fs,ms,_=struct.unpack_from('<II6Q',ph,j*stride)
                        if typ!=1 or fl&2:continue
                        start=addr+va-va0;assert start%4096==0 and va>=va0 and va+fs<=va0+sz
                        pm.seek(start//4096*8)
                        for (x,) in struct.iter_unpack('<Q',pm.read(fs//4096*8)):
                            if not x>>63:continue
                            pfn=x&((1<<55)-1);assert pfn!=0,'masked PFN'
                            pfns[name].add(pfn);present+=1;file+=(x>>61)&1
                            if not (x>>61)&1:exclusive+=(x>>56)&1
                    rows.append(dict(process=proc,pid=p.name,library=name,present_kib=present*4,file_or_shared_kib=file*4,exclusive_anon_kib=exclusive*4))
        except (FileNotFoundError,ProcessLookupError):continue
    assert all(len(pfns[name]) for name in names)
    return rows,[dict(library=name,processes=sum(x['library']==name for x in rows),distinct_resident_kib=len(pfns[name])*4) for name in names]


if __name__=='__main__':
    import argparse,csv
    parser=argparse.ArgumentParser(description="Count mapped physical pages of two qualified loader dependencies; no PFNs are written.")
    parser.add_argument('elf',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args()
    per_process,unique=measure(args.elf)
    args.output.mkdir()
    for name,rows in [('processes',per_process),('unique',unique)]:
        with (args.output/(name+'.csv')).open('w',newline='') as f:
            w=csv.DictWriter(f,rows[0],lineterminator='\n');w.writeheader();w.writerows(rows)
