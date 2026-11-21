# Read only ELF/surface bookkeeping; count resident pages without reading image pixels.
from pathlib import Path
import os,struct,hashlib,collections,re,subprocess,sys,csv
from contextlib import ExitStack
out=Path(sys.argv[1])
known={'342508c0dde9aaeb549d230c4bc4b300eb7f108f77ee95bff9fb2a14bb7c443d',
       '52da392d9b6bed1c28a1ce9cd4fe2426adbd6f61e007dbe90181b734e7c679ba',
       '2e43e41280440386044eb59618069f4b614d4499c9f57eaa91c8d9f20f603b2c'}
elf=Path('out/libEGL_angle.so')
sha=hashlib.sha256(elf.read_bytes()).hexdigest()
assert sha in known
def match(p): return hashlib.sha256(p.read_bytes()).hexdigest()==sha
assert match(elf) and os.sysconf('SC_PAGE_SIZE')==4096
syms={r[3]:(int(r[0],16),int(r[1],16)) for l in subprocess.check_output(['nm','-S',elf],text=True).splitlines() if len(r:=l.split())==4}
libs=[]; caches=[]; detail=[]
for p in sorted(Path('/proc').glob('[0-9]*')):
 try:
  if 'lxc.payload.waydroid' not in (p/'cgroup').read_text():continue
  mp=(p/'maps').read_text()
  m=re.search(r'^([a-f0-9]+)-\w+ r--p 00000000 (\S+) (\d+) +(/system/lib64/libEGL_angle.so)$',mp,re.M)
  if not m:continue
  ef=p/'root'/m[4][1:];st=ef.stat()
  assert st.st_ino==int(m[3]) and match(ef)
  base=int(m[1],16)
  name=(p/'cmdline').read_bytes().split(b'\0')[0].decode().split('/')[-1]
  with ExitStack() as stack:
   mem=stack.enter_context((p/'mem').open('rb',buffering=0)); pag=stack.enter_context((p/'pagemap').open('rb',buffering=0))
   def meta(k):
    a,n=syms[k];return os.pread(mem.fileno(),n,base+a)
   def pages(a,end):
    lo=(a+4095)//4096;hi=end//4096
    return (r[0] for r in struct.iter_unpack('<Q',os.pread(pag.fileno(),8*max(hi-lo,0),lo*8)))
   def resident(a,n):return sum(x>>63 for x in pages(a,a+n))*4 if a and n else 0
   nl,=struct.unpack('<i',meta('g_nlibs')); assert 0<=nl<=16
   data=meta('g_libs');assert len(data)==264*16
   for i in range(nl):
    ent=data[i*264:(i+1)*264];ptr,vmin,length=struct.unpack_from('<3Q',ent)
    fn=ent[136:].split(b'\0')[0].decode();f=p/'root'/fn.lstrip('/')
    with f.open('rb') as fo:
     eh=fo.read(64); assert eh[:4]==b'\x7fELF';phoff=struct.unpack_from('<Q',eh,32)[0];esize,cnt=struct.unpack_from('<HH',eh,54)
     fo.seek(phoff);ph=fo.read(esize*cnt)
    an=file=0
    for j in range(cnt):
     typ,flags,offset,va,_,filesz,memsz,align=struct.unpack_from('<II6Q',ph,j*esize)
     if typ!=1 or flags&2:continue
     assert va>=vmin and va-vmin+filesz<=length
     for x in pages(ptr+va-vmin,ptr+va-vmin+filesz):
      if not x>>63:continue
      if x>>61&1:file+=4
      elif x>>56&1:an+=4
    libs.append((name,p.name,Path(fn).name,an,file))
   scratch=current=last=0
   ss=meta('scratch_slots');assert len(ss)==80*8
   for i in range(8):
    a,b,n,cur=struct.unpack_from('<QQQi',ss,80*i+8)
    assert 0<=cur<=1 and n<=2**32
    sizes=[resident(a,n),resident(b,n)];scratch+=sum(sizes)
    if n: detail.append([name,i,n//1024,cur,*struct.unpack_from("<2i",ss,80*i+36),*sizes,struct.unpack_from("<i",ss,80*i+72)[0]])
    if not struct.unpack_from('<i',ss,80*i+72)[0]: current+=sizes[cur]
   ns,=struct.unpack('<i',meta('g_nsurf')); assert 0<=ns<=64
   tab=meta('g_surf');assert len(tab)==152*64
   for i in range(ns):
    a,n=struct.unpack_from('<QQ',tab,152*i+136);assert n<=2**32;last+=resident(a,n)
   caches.append((name,p.name,nl,scratch,current,last))
 except (FileNotFoundError,ProcessLookupError):print('process/file changed:',p,file=sys.stderr)
for fn,header,rows in [('loader.csv','process,pid,library,exclusive_anon_ro_kib,resident_file_or_sharedanon_ro_kib',libs),('cache.csv','process,pid,libs,scratch_resident_kib,idle_write_side_kib,surface_last_resident_kib',caches)]:
 with (out/fn).open('w') as f:
  w=csv.writer(f);w.writerow(header.split(','));w.writerows(rows)
for v in caches:print(*v)
agg=collections.Counter()
for _,_,lib,a,_ in libs:agg[lib]+=a
print(agg)

with (out/"slots.csv").open("w") as f:
 w=csv.writer(f);w.writerow("process,slot,capacity_kib,write_side,ok_0,ok_1,resident_0_kib,resident_1_kib,busy".split(","));w.writerows(detail)
