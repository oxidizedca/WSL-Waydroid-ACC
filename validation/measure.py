"""WSL root. Compare the same deployed binary, without concurrent video capture."""
import csv
import hashlib
import math
import os
import re
import subprocess as sp
import sys
import time
from collections import defaultdict
from contextlib import ExitStack
from pathlib import Path
from statistics import median

out=Path(sys.argv[1])
out.mkdir()
android=["lxc-attach","-P","/var/lib/waydroid/lxc","-n","waydroid","--"]
def run(*args):
    return sp.check_output([*android,*args],text=True).strip()
def prop(key,value):
    run("setprop","debug.waydroid.acc_"+key,value)
def write(path,value):
    with os.fdopen(os.open(path,os.O_WRONLY|os.O_APPEND),"w") as f:
        f.write(value+"\n")
marker=Path("/run/waydroid-acc-query-on")
tracefs=Path("/sys/kernel/tracing")
group=f"wd_measure_{os.getpid()}"
inst=tracefs/"instances"/group
rows=[]
def collect(rep,mode,scene,data):
    for op,v in data.items():
        if v:
            rows.append([rep,mode,scene,op,len(v),round(median(v),4),round(sorted(v)[math.ceil(.95*len(v))-1],4)])
    with (out/"sample.csv").open("w",newline="") as f:
        wr=csv.writer(f)
        wr.writerow(["run","bind_query_pool","scene","metric","n","p50_ms","p95_ms"])
        wr.writerows(rows)

def probe(stack):
    pid=sp.check_output(["pgrep","-xo","surfaceflinger"],text=True).strip()
    egl=Path(f"/proc/{pid}/root/system/lib64/libEGL_angle.so")
    # The compiler can inline present into eglSwapBuffers. Match the verified
    # publication site and request stack layout for each installed binary.
    egl_builds = {
        "bd8314e1c4a624c0d9247b9cac08d96e53fb44647a99448ce018d68e5aec4d79": ("present", 0x34f5a, 0x10),
        "ce6b53d26ce4f26032299a50ed437600ab3231b75abde2c4b49802b8e08ebb5c": ("eglSwapBuffers", 0x2810a, 0x10),
    }
    broker_builds = {
        "7f16be3b4d27bcbdf9254bc15539485689ae0296b5ed62055d329144fd860d98": (0x4b17, 0xc0),
        "490f51264ff77eed9ff808c50396059d0afe307dd1ea3a7b4866cfc9b3dff6f2": (0x4b0f, 0xa0),
    }
    broker_pid=sp.check_output(["systemctl","show","-p","MainPID","--value","waydroid-acc-input.service"],text=True).strip()
    broker=Path(f"/proc/{broker_pid}/exe")
    symbol,send_pc,send_base=egl_builds[hashlib.sha256(egl.read_bytes()).hexdigest()]
    peek_pc,peek_base=broker_builds[hashlib.sha256(broker.read_bytes()).hexdigest()]
    listing=sp.check_output(["objdump","-d",f"--disassemble={symbol}",egl],text=True)
    calls=re.findall(r"^\s*([0-9a-f]+):.*\bcall\s+[0-9a-f]+ <frame_packet>\n\s*([0-9a-f]+):",listing,re.M)
    assert len(calls)==2, calls
    segs=sp.check_output(["readelf","-lW",egl],text=True)
    loads=[tuple(int(x,16) for x in row) for row in re.findall(r"LOAD\s+(0x\w+)\s+(0x\w+)\s+0x\w+\s+(0x\w+)\s+0x\w+\s+R E",segs)]
    inst.mkdir()
    stack.callback(inst.rmdir)
    for name,v in [("pub_start",calls[0][0]),("pub_end",calls[1][1])]:
        pc=int(v,16)
        off=next(a+pc-b for a,b,size in loads if b<=pc<b+size)
        write(tracefs/"uprobe_events",f"p:{group}/{name} {egl}:0x{off:x}")
        stack.callback(write,tracefs/"uprobe_events",f"-:{group}/{name}")
    for binary,event,pc,base in [(egl,"get_send",send_pc,send_base),
                                 (broker,"get_peek",peek_pc,peek_base)]:
        headers=sp.check_output(["readelf","-lW",binary],text=True)
        for a,b,size in re.findall(r"LOAD\s+(0x\w+)\s+(0x\w+)\s+0x\w+\s+(0x\w+)\s+0x\w+\s+R E",headers):
            if int(b,16)<=pc<int(b,16)+int(size,16): break
        else: raise ValueError(pc)
        off=int(a,16)+pc-int(b,16)
        fields=" ".join(f"{k}=+{base+n}(%sp):{fmt}" for k,n,fmt in [
            ("phase",4,"u32"),("serial",20,"u32"),("hi",24,"u32"),("id",32,"u64"),("lo",40,"u32")])
        write(tracefs/"uprobe_events",f"p:{group}/{event} {binary}:0x{off:x} {fields}")
        stack.callback(write,tracefs/"uprobe_events",f"-:{group}/{event}")
        (inst/"events"/group/event/"filter").write_text("phase==30")
    (inst/"trace_clock").write_text("boot")
    (inst/"buffer_size_kb").write_text("1024")
    (inst/"events"/group/"enable").write_text("1")
    stack.callback((inst/"events"/group/"enable").write_text,"0")
    (inst/"tracing_on").write_text("0")

def sweep(rep,mode,scene,diag):
    settings=scene=="settings"
    pkg="com.android.settings" if settings else "com.android.systemui"
    front=pkg if settings else "com.android.calculator2"
    run("cmd","statusbar","collapse")
    run("am","start",*(["-a","android.settings.SETTINGS"] if settings else ["-n",front+"/.Calculator"]))
    time.sleep(.8)
    assert any(front+"/" in line for line in run("dumpsys","activity","activities").splitlines() if "topResumedActivity=" in line)
    run("dumpsys","gfxinfo",pkg,"reset")
    key=f"{rep}-{mode}-{scene}"
    data=defaultdict(list)
    if diag:
        (inst/"trace").write_text("")
        (inst/"tracing_on").write_text("1")
        log=(out/(key+".log")).open("w")
        reader=sp.Popen([*android,"logcat","-T","1","-v","brief","-s","d3d12img:I"],stdout=log)
    try:
        for _ in range(3):
            if settings:
                for y1,y2 in [("855","280"),("280","855")]:
                    run("input","swipe","320",y1,"320",y2,"380")
                    time.sleep(.23)
            else:
                for cmd in ["expand-notifications","collapse"]:
                    run("cmd","statusbar",cmd)
                    time.sleep(.64)
    finally:
        if diag:
            (inst/"tracing_on").write_text("0")
            reader.terminate()
            reader.wait(timeout=3)
            log.close()
    if diag:
        pending={}
        stats="".join(p.read_text() for p in inst.glob("per_cpu/cpu*/stats"))
        assert not re.search(r"(?:overrun|dropped events): [1-9]",stats)
        trace=(inst/"trace").read_text()
        pending_get={}
        for line in trace.splitlines():
            m=re.search(r"(\d+\.\d+): (get_send|get_peek): .*?phase=30 serial=(\d+) hi=(\d+) id=(\d+) lo=(\d+)",line)
            if not m: continue
            ts,event,*identity=m.groups()
            packet=tuple(identity)
            if event=="get_send": pending_get[packet]=float(ts)
            elif packet in pending_get: data["get_to_dispatch"].append((float(ts)-pending_get.pop(packet))*1000)
        assert data["get_to_dispatch"],key
        for line in trace.splitlines():
            m=re.search(r"-(\d+)\s+\[\d+\]\s+\S+\s+(\d+\.\d+): (pub_start|pub_end):",line)
            if not m: continue
            tid,ts,name=m.groups()
            if name=="pub_start": pending[tid]=float(ts)
            elif tid in pending: data["publication_ack"].append((float(ts)-pending.pop(tid))*1000)
        for line in (out/(key+".log")).read_text().splitlines():
            if "texture-cache " not in line: continue
            d={k:int(v) for k,v in re.findall(r"(\w+)=(\d+)",line)}
            if "spare" in d:
                for op in ["create","submit","handle","lock"]: data[op].append(d[op]/1e6)
                data["export_stages"].append(sum(d[k] for k in ["create","submit","handle"])/1e6)
                if d["spare"]: data["prepared_export"].append(d["create"]/1e6)
            elif "bind" in d:
                data["cached_bind" if d["cached"] else "fresh_bind"].append(d["bind"]/1e6)
                if not d["cached"]: data["fresh_get"].append(d["get"]/1e6)
                if d["hit"]: data["resource_hit"].append(d["bind"]/1e6)
        assert data["export_stages"] and data["publication_ack"],key
    else:
        raw=run("dumpsys","gfxinfo",pkg,"framestats")
        (out/(key+".txt")).write_text(raw)
        frames={}
        for line in raw.splitlines():
            if line.startswith("Flags,"): fields=line.split(",")
            if line.startswith("0,"):
                d=dict(zip(fields,line.split(",")))
                frame=int(d["FrameTimelineVsyncId"])
                elapsed=(int(d["FrameCompleted"])-int(d["IntendedVsync"]))/1e6
                if 0<elapsed<10000: frames[frame]=max(frames.get(frame,0),elapsed)
        assert frames,key
        data["hwui_frame"]=list(frames.values())
    collect(rep,mode,scene,data)
    print(key,flush=True)

prev=[run("getprop","debug.waydroid.acc_"+p) for p in ["bind","pool","trace"]]
old=marker.exists()
try:
    for rep,modes in [("clean1",["000","100","010","001","111","101"]),("clean2",["101","111","001","010","100","000"]),("diag",["101","111","001","100","010","000"])]:
        diag=rep=="diag"
        if len(sys.argv)>2 and rep not in sys.argv[2:]: continue
        with ExitStack() as stack:
            if diag: probe(stack)
            prop("trace",str(int(diag)))
            for mode in modes:
                prop("bind",mode[0]);prop("pool",mode[2])
                if mode[1]=="1": marker.touch()
                else: marker.unlink(missing_ok=True)
                for scene in ["settings","shade"]: sweep(rep,mode,scene,diag)
finally:
    for name,value in zip(["bind","pool","trace"],prev): prop(name,value)
    if old: marker.touch()
    else: marker.unlink(missing_ok=True)
    run("input","keyevent","KEYCODE_HOME")
