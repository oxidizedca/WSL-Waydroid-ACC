"""WSL root: memory footprint at settled UI checkpoints, no cache flushing or app killing."""
import csv
import os
import subprocess as sp
import sys
import time
from pathlib import Path

def run(*args):
    return sp.check_output(args,text=True).strip()

def android(*args):
    return run("lxc-attach","-P","/var/lib/waydroid/lxc","-n","waydroid","--",*args)

def rollup(pid):
    return {k:int(v.split()[0])*1024 for k,v in
            (line.split(":",1) for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines()[1:])}

def write_rows(path,rows):
    with path.open("w",newline="") as f:
        wr=csv.DictWriter(f,rows[0],lineterminator="\n");wr.writeheader();wr.writerows(rows)

def resources():
    cg=Path("/sys/fs/cgroup")
    units=("waydroid-acc-input","waydroid-acc-consumer","waydroid-acc-weston","waydroid-container")
    groups={"payload":cg/"lxc.payload.waydroid","monitor":cg/"lxc.monitor.waydroid"}
    groups.update({u:cg/run("systemctl","show",u,"-p","ControlGroup","--value").lstrip("/") for u in units})
    assert all(p != cg and (p/"memory.current").exists() for p in groups.values())
    assert all(not g.is_relative_to(h) for n,g in groups.items() for m,h in groups.items() if m != n)
    # These session/network helpers can share cgroups with unrelated WSL processes.
    helper=set()
    for p in Path("/proc").glob("[0-9]*"):
        try:
            cmd=(p/"cmdline").read_bytes().rstrip(b"\0").split(b"\0")
            if not ((cmd[-3:]==[b"/usr/bin/waydroid",b"session",b"start"]) or
                    (cmd[0].endswith(b"dnsmasq") and b"--pid-file=/run/waydroid-lxc/dnsmasq.pid" in cmd)): continue
            group=cg/(p/"cgroup").read_text().strip().split("::",1)[1].lstrip("/")
            if not any(group.is_relative_to(g) for g in groups.values()): helper.add(int(p.name))
        except (OSError,IndexError): pass
    return groups,helper

def footprint(groups,helper):
    r={}
    helper_pss=0
    for pid in helper:
        try: helper_pss+=rollup(pid)["Pss"]
        except FileNotFoundError: pass
    total=helper_pss; cache=swap=0
    for name,g in groups.items():
        cur=int((g/"memory.current").read_text())
        stat=dict(l.split() for l in (g/"memory.stat").read_text().splitlines())
        r[name]=cur;total+=cur;cache+=int(stat["inactive_file"])
        swap+=int((g/"memory.swap.current").read_text())
        if name=="payload":
            for k in ("anon","file","shmem","kernel"):r["payload_"+k]=int(stat[k])
    r.update(helper_pss=helper_pss,total=total,inactive_file=cache,working=total-cache,swap=swap)
    return r

def main(out):
    out.mkdir()
    if not any("topResumedActivity=" in s and "com.android.launcher3/" in s
               for s in android("dumpsys","activity","activities").splitlines()):
        raise SystemExit("Start from Launcher; refusing to switch away from an active app")
    groups,helper=resources()
    broker=int(run("systemctl","show","waydroid-acc-input","-p","MainPID","--value"))
    rows=[];procs=[]
    def checkpoint(stage):
        for i in range(5):
            r=dict(stage=stage,sample=i)
            r.update(footprint(groups,helper))
            planes={}; dxg=0
            for fd in Path(f"/proc/{broker}/fd").iterdir():
                try:
                    target=os.readlink(fd)
                    if target.startswith("/dmabuf:"):
                        st=fd.stat();planes[st.st_dev,st.st_ino]=st.st_size
                    elif target=="anon_inode:dxgresource":dxg+=1
                except FileNotFoundError: pass
            r.update(broker_pss=rollup(broker)["Pss"],planes=len(planes),plane_bytes=sum(planes.values()),dxg_fds=dxg)
            rows.append(r)
            if i<4: time.sleep(.4)
        if stage in ("desktop","home_idle"):
            for f in Path("/proc").glob("[0-9]*/cgroup"):
                try:
                    if not f.read_text().startswith("0::/lxc.payload.waydroid/"):continue
                    pid=int(f.parent.name); mem=rollup(pid)
                    name=(f.parent/"cmdline").read_bytes().split(b"\0")[0].decode(errors="replace")
                    procs.append(dict(stage=stage,pid=pid,name=name,pss=mem["Pss"],anon_pss=mem["Pss_Anon"]))
                except (OSError,KeyError): pass
        write_rows(out/"samples.csv",rows)
    try:
        time.sleep(5);checkpoint("desktop")
        android("am","start","-W","-n","com.android.calculator2/.Calculator");time.sleep(2)
        checkpoint("calculator")
        for i in range(12):
            android("cmd","statusbar","expand-notifications");time.sleep(.65)
            android("cmd","statusbar","collapse");time.sleep(.65)
        checkpoint("after_shade")
        android("input","keyevent","KEYCODE_HOME");time.sleep(2);checkpoint("home")
        time.sleep(25);checkpoint("home_idle")
        assert int(run("systemctl","show","waydroid-acc-input","-p","MainPID","--value"))==broker
        write_rows(out/"processes.csv",procs)
    finally:
        android("cmd","statusbar","collapse");android("input","keyevent","KEYCODE_HOME")

if __name__=="__main__":main(Path(sys.argv[1]))
