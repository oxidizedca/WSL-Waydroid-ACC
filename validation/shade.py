"""WSL root: measure one 1080p notification-shade run without probes or screen capture."""
import argparse
import csv
import io
import math
import os
from pathlib import Path
import subprocess as sp
import time
from statistics import median

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    folder = args.output
    folder.mkdir()
    android = ["lxc-attach", "-P", "/var/lib/waydroid/lxc", "-n", "waydroid", "--"]
    def run(*cmd):
        return sp.check_output([*android, *cmd], text=True).strip()
    assert "Physical size: 1920x1080" in run("wm", "size")
    assert run("getprop", "debug.waydroid.acc_trace") in ("", "0")
    tp = next((Path.home()/".local/share/perfetto/prebuilts").glob("trace_processor_shell-*"))
    broker = sp.check_output(["systemctl", "show", "-p", "MainPID", "--value",
                              "waydroid-acc-input.service"], text=True).strip()
    (folder/"broker.sha256").write_text(sp.check_output(["sha256sum", f"/proc/{broker}/exe"], text=True))
    trace = f"/data/misc/perfetto-traces/wd-shade-{os.getpid()}.pftrace"
    cfg = """buffers { size_kb: 65536 fill_policy: RING_BUFFER }
duration_ms: 30000
data_sources { config { name: "android.surfaceflinger.frametimeline" } }
data_sources { config { name: "linux.process_stats" process_stats_config { scan_all_processes_on_start: true } } }
"""
    intervals = []
    clock = lambda: time.clock_gettime_ns(time.CLOCK_BOOTTIME)
    def motion(op):
        lo = clock()
        run("cmd", "statusbar", op)
        time.sleep(.64)
        return lo, clock()
    pid = None
    try:
        run("cmd", "statusbar", "collapse")
        run("am", "start", "-W", "-n", "com.android.calculator2/.Calculator")
        assert any("com.android.calculator2/" in s and "topResumedActivity=" in s
                   for s in run("dumpsys", "activity", "activities").splitlines())
        ops = ["expand-notifications", "collapse"]
        for _ in range(2):
            for op in ops: motion(op)
        reply = sp.check_output([*android, "perfetto", "--background-wait", "--txt", "-c", "-", "-o", trace],
                                input=cfg, text=True)
        pid = next(s for s in reply.splitlines() if s.isdigit())
        for rep in range(6):
            for op in ops: intervals.append((rep, op, *motion(op)))
        run("kill", "-INT", pid)
        for _ in range(80):
            if sp.run([*android, "test", "-e", "/proc/"+pid]).returncode: break
            time.sleep(.1)
        else: raise RuntimeError("trace did not stop")
        pid = None
        with (folder/"trace.pftrace").open("wb") as out:
            sp.run([*android, "cat", trace], stdout=out, check=True)
    finally:
        if pid: sp.run([*android, "kill", "-INT", pid])
        run("rm", "-f", trace)
        run("cmd", "statusbar", "collapse")
        run("input", "keyevent", "KEYCODE_HOME")
    def query(sql):
        return list(csv.DictReader(io.StringIO(sp.check_output([tp,"-Q",sql,folder/"trace.pftrace"],text=True))))
    assert not query("select name from stats where name in ('traced_buf_chunks_overwritten','ftrace_cpu_overrun_end') and value>0")
    frames = [(int(s['ts']), int(s['dur'])) for s in query("""
        select ts,dur from actual_frame_timeline_slice join process using(upid)
        where process.name in ('surfaceflinger','/system/bin/surfaceflinger') and surface_frame_token is null and dur>0
    """)]
    with (folder/"frames.csv").open("w", newline="") as f:
        wr = csv.writer(f); wr.writerow(["cycle","action","ts_ns","duration_ns"])
        for rep,op,lo,hi in intervals:
            selected=[(ts,dur) for ts,dur in frames if lo<=ts and ts+dur<=hi]
            assert selected, (rep,op)
            wr.writerows((rep,op,ts,dur) for ts,dur in selected)
    with (folder/"summary.csv").open("w", newline="") as f:
        wr=csv.writer(f);wr.writerow(["action","frames","p50_ms","p95_ms","over_16_67"])
        rows=list(csv.DictReader((folder/"frames.csv").read_text().splitlines()))
        for op in [*ops, "both"]:
            v=sorted(int(s["duration_ns"]) / 1e6 for s in rows if op=="both" or s["action"]==op)
            wr.writerow([op,len(v),round(median(v),3),round(v[math.ceil(.95*len(v))-1],3),sum(x>1000/60 for x in v)])
    print((folder/"summary.csv").read_text(),end="")
if __name__ == "__main__":
    main()
