"""WSL root; version-pinned userspace probes. No changes to deployed binaries."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess as sp
import time

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument("out", type=Path)
ap.add_argument("--detail", action="store_true")
ap.add_argument("--slots", action="store_true", help="inspect the 64 eviction candidates (separate from timing)")
args = ap.parse_args()
args.out.mkdir()  # refuse to overwrite another measurement
android = ["lxc-attach", "-P", "/var/lib/waydroid/lxc", "-n", "waydroid", "--"]
def run(*argv):
    return sp.check_output(argv, text=True).strip()
def a(*argv):
    return run(*android, *argv)

sf = run("pgrep", "-xo", "surfaceflinger")
bp = run("systemctl", "show", "waydroid-acc-input", "-p", "MainPID", "--value")
shim = Path(f"/proc/{sf}/root/system/lib64/libEGL_angle.so")
broker = Path(f"/proc/{bp}/exe").resolve(strict=True)
for file, want in [(shim, "87397532a9ab86439fbf40c1d28e53e8ad312a0ee8feeecb4f5ff43087429daa"),
                   (broker, "f080048f14bb000e2312ec1b6b2edd6657871f58612ac3261923744e967e6faa")]:
    if hashlib.sha256(file.read_bytes()).hexdigest() != want:
        raise SystemExit(f"Different binary: audit instruction offsets before using {file}")
if "Physical size: 1920x1080" not in a("wm", "size") or a("getprop","debug.waydroid.acc_trace") not in ("","0"):
    raise SystemExit("Expected 1080p with diagnostic logging disabled")
if not any("topResumedActivity=" in l and "com.android.launcher3/" in l
           for l in a("dumpsys","activity","activities").splitlines()):
    raise SystemExit("Start from the launcher; not switching away from an active app")

tr = Path("/sys/kernel/tracing")
group = f"wdget{os.getpid()}"
ins = tr/"instances"/group
names = []
def control(s):
    fd = os.open(tr/"uprobe_events", os.O_APPEND | os.O_WRONLY)
    try: os.write(fd, (s+"\n").encode())
    finally: os.close(fd)
def probe(name, file, off, fmt="", ret=False):
    control(f"{'r' if ret else 'p'}:{group}/{name} {file}:0x{off:x} {fmt}".rstrip())
    names.append(name)
def setopt(k,v):
    (ins/k).write_text(str(v))
def motion(op):
    a("cmd","statusbar",op)
    time.sleep(.68)

ins.mkdir()
switched = False
try:
    setopt("tracing_on",0)
    setopt("trace_clock","boot")
    setopt("buffer_size_kb",2048)
    # File offsets, not symbol VMAs. The shim's executable PT_LOAD differs by 0x1000.
    probe("rq",shim,0x270a0,"phase=%di:u32 id=%si:u64 nonce=%cx:x64 serial=%r8:u32")
    probe("send",shim,0x272da)
    probe("end",shim,0x270a0,"rc=$retval:u32",True)
    probe("serve",broker,0x4b70)
    probe("served",broker,0x4b70,ret=True)
    # Immediately after a full-length MSG_PEEK. frame_frame at rsp+0xa0.
    probe("peek",broker,0x4c0f,"phase=+0xa4(%sp):u32 id=+0xc0(%sp):u64 hi=+0xb8(%sp):x32 lo=+0xc8(%sp):x32 serial=+0xb4(%sp):u32 w=+0xa8(%sp):u32 h=+0xac(%sp):u32")
    probe("reply",broker,0x52ec,"phase=+0xa4(%sp):u32 lazy=+0xbc(%sp):u32")
    if args.slots:
        # At this instruction r12 = entries, r15 = chosen slot, edx = next_entry.
        # A 32-bit tex and a 32-bit lazy flag per slot, split across two events.
        for tag,field in (("occupied",0xf4),("lazy",0xd8)):
            probe("slots_"+tag,broker,0x5eae,"seq=%dx:u32 "+" ".join(
                f"s{i}=+0x{i*0x128+field:x}(%r12):u32" for i in range(64)))
    if args.detail:
        for tag,off in [("import",0x3dc0),("mirror",0x4950),("release",0x4870),
                        ("digest",0x4680),("interleave",0x6380)]:
            probe(tag+"_in",broker,off)
            probe(tag+"_out",broker,off,ret=True)
        if args.slots:
            probe("readback_in",broker,0x4b20)
            probe("readback_out",broker,0x4b26)
        for tag,lo,hi in [("fence",0x56ca,0x56cf),("norm_query",0x5bcf,0x5bd5),
                          ("finish",0x5c00,0x5c06),("draw",0x5d1b,0x5d21),
                          ("vis_query",0x5d48,0x5d4e),("retain_ack",0x6098,0x609d)]:
            probe(tag+"_in",broker,lo)
            probe(tag+"_out",broker,hi)
    setopt("events/"+group+"/enable",1)
    switched = True
    a("am","start","-W","-n","com.android.calculator2/.Calculator")
    for _ in range(2):
        for op in ("expand-notifications","collapse"): motion(op)
    setopt("tracing_on",1)
    for cycle in range(6):
        for op in ("expand-notifications","collapse"):
            setopt("trace_marker",f"motion {cycle} {op}")
            motion(op)
    setopt("trace_marker","done")
    setopt("tracing_on",0)
    (args.out/"trace.txt").write_bytes((ins/"trace").read_bytes())
    for s in (ins/"per_cpu").glob("cpu*/stats"):
        for line in s.read_text().splitlines():
            if line.split(":")[0].strip() in ("overrun","dropped events") and int(line.split(":")[1]):
                raise RuntimeError(f"lost events: {s}: {line}")
finally:
    try:
        setopt("tracing_on",0)
        if names: setopt("events/"+group+"/enable",0)
    finally:
        try: ins.rmdir()
        finally:
            for name in reversed(names): control(f"-:{group}/{name}")
            if switched:
                a("cmd","statusbar","collapse")
                a("input","keyevent","KEYCODE_HOME")
