"""Capture SF's primary display passes and FrameTimeline under reversed mode ordering."""
import csv
import io
import math
import os
import re
import subprocess as sp
import sys
import time
from collections import defaultdict
from pathlib import Path
from statistics import median

folder=Path(sys.argv[1]);folder.mkdir()
android=["lxc-attach","-P","/var/lib/waydroid/lxc","-n","waydroid","--"]
def run(*args):
    return sp.check_output([*android,*args],text=True).strip()
cfg=f"/data/local/tmp/wd-display-{os.getpid()}.cfg"
trace=f"/data/misc/perfetto-traces/wd-display-{os.getpid()}.pftrace"
text='''buffers { size_kb: 131072 fill_policy: RING_BUFFER }
duration_ms: 220000
data_sources { config { name: "android.surfaceflinger.frametimeline" } }
data_sources { config { name: "linux.process_stats" process_stats_config { scan_all_processes_on_start: true } } }
data_sources { config { name: "linux.ftrace" ftrace_config { atrace_categories: "gfx" ftrace_events: "ftrace/print" } } }
data_sources { config { name: "track_event" track_event_config { disabled_categories: "*" enabled_categories: "gfx" } } }
'''
sp.run([*android,"sh","-c","cat > "+cfg],input=text,text=True,check=True)
marker=Path("/run/waydroid-acc-query-on"); old=marker.exists()
orig={k:run("getprop","debug.waydroid.acc_"+k) for k in ["bind","pool","trace"]}
clock=lambda:time.clock_gettime_ns(time.CLOCK_BOOTTIME)
windows=[]
pid=None
try:
    run("setprop","debug.waydroid.acc_trace","0")
    reply=run("perfetto","--background-wait","--txt","-c",cfg,"-o",trace)
    pid=re.findall(r'^\d+$',reply,re.M)[-1]
    for rep,modes in [(1,["000","101","111"]),(2,["111","101","000"])]:
        for mode in modes:
            run("setprop","debug.waydroid.acc_bind",mode[0])
            run("setprop","debug.waydroid.acc_pool",mode[2])
            if mode[1]=="1": marker.touch()
            else: marker.unlink(missing_ok=True)
            for name in ["settings","shade"]:
                run("cmd","statusbar","collapse")
                run("am","start",*(["-a","android.settings.SETTINGS"] if name=="settings" else ["-n","com.android.calculator2/.Calculator"]))
                time.sleep(.85)
                start=clock()
                for i in range(3):
                    if name=="settings":
                        for y,z in [("855","280"),("280","855")]:
                            run("input","swipe","320",y,"320",z,"380")
                            time.sleep(.23)
                    else:
                        for op in ["expand-notifications","collapse"]:
                            run("cmd","statusbar",op)
                            time.sleep(.64)
                windows.append((rep,mode,name,start,clock()))
    run("kill","-INT",pid)
    for _ in range(60):
        if sp.run([*android,"test","-e","/proc/"+pid]).returncode: break
        time.sleep(.1)
    pid=None
    with (folder/"trace.pftrace").open("wb") as out:
        sp.run([*android,"cat",trace],stdout=out,check=True)
finally:
    if pid: sp.run([*android,"kill","-INT",pid])
    run("rm","-f",cfg,trace)
    for k,v in orig.items(): run("setprop","debug.waydroid.acc_"+k,v)
    if old: marker.touch()
    else: marker.unlink(missing_ok=True)
    run("input","keyevent","KEYCODE_HOME")
with (folder/"windows.csv").open("w",newline="") as f: csv.writer(f).writerows(windows)
tp=next((Path.home()/".local/share/perfetto/prebuilts").glob("trace_processor_shell-*"))
q="""select s.ts,s.dur,'physical_draw' as name from slice s where name='drawLayersInternal for Primary display (0)' and dur>0
union all select a.ts,a.dur,'display_frame' from actual_frame_timeline_slice a join process p using(upid)
where (p.name='/system/bin/surfaceflinger' or p.name='surfaceflinger') and a.dur>0 and a.surface_frame_token is null
order by ts"""
def query(sql):
    return list(csv.DictReader(io.StringIO(sp.check_output([tp,"-Q",sql,folder/"trace.pftrace"],text=True))))
assert not query("select name from stats where name in ('traced_buf_chunks_overwritten','ftrace_cpu_overrun_end') and value>0"), "incomplete trace"
samples=query(q)
assert samples
with (folder/"display.csv").open("w",newline="") as f:
    wr=csv.writer(f);wr.writerow(["run","bind_query_pool","scene","metric","n","p50_ms","p95_ms"])
    for rep,mode,name,lo,hi in windows:
        values=defaultdict(list)
        for s in samples:
            if lo<=int(s["ts"]) and int(s["ts"])+int(s["dur"])<=hi: values[s["name"]].append(int(s["dur"])/1e6)
        assert values["physical_draw"] and values["display_frame"]
        for key,v in values.items():
            wr.writerow([rep,mode,name,key,len(v),round(median(v),3),round(sorted(v)[math.ceil(.95*len(v))-1],3)])
