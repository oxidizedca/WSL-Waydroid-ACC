"""Compare two clean boots: run as WSL root after boot, from the Android desktop."""
import argparse
import os
import re
import time
from pathlib import Path
from memory import android, footprint, resources, rollup, write_rows

def front():
    text=android("dumpsys","activity","activities")
    return next((s.split(" u0 ")[1].split()[0] for s in text.splitlines()
                 if "topResumedActivity=" in s and " u0 " in s),"")

def main(out, ui_only=False):
    out.mkdir()
    groups,helper=resources()
    samples=[]; processes=[]
    def checkpoint(label,package):
        activity=front()
        if not activity.startswith(package+"/"):
            raise RuntimeError(f"{label}: unexpected foreground: {activity}")
        for i in range(5):
            samples.append(dict(stage=label,i=i,front=activity,**footprint(groups,helper)))
            if i<4: time.sleep(1)
        write_rows(out/"samples.csv",samples)
        for p in Path("/proc").glob("[0-9]*"):
            try:
                if "/lxc.payload.waydroid" not in (p/"cgroup").read_text():continue
                name=(p/"cmdline").read_bytes().split(b"\0")[0].decode(errors="replace")
                maps=(p/"maps").read_text()
                if "/system/lib64/libEGL_angle.so" not in maps:continue
                m=rollup(p.name)
                lib_fds=0
                for fd in (p/"fd").iterdir():
                    try: path=os.readlink(fd)
                    except FileNotFoundError: continue
                    if path.startswith(("/data/local/tmp/mesa/", "/data/local/tmp/wsl_drivers/",
                                        "/data/local/tmp/lib")) and ".so" in path:
                        lib_fds+=1
                processes.append(dict(stage=label,name=name,pid=p.name,pss=m["Pss"],anon=m["Pss_Anon"],
                                      loader_lib_fds=lib_fds,
                                      gallium_rx=int(bool(re.search(r" r-xp .*?/libgallium-26\.0\.8\.so$",maps,re.M))),
                                      umd_rx=int(bool(re.search(r" r-xp .*?/amdxc64\.so$",maps,re.M)))))
            except (OSError,KeyError):pass
        if processes:write_rows(out/"processes.csv",processes)
        print(label,activity,flush=True)
    def start(component):
        result=android("am","start","-W","-n",component)
        if "Error" in result:raise RuntimeError(result)
    launcher="com.android.launcher3"
    android("input","keyevent","KEYCODE_HOME")
    time.sleep(25);checkpoint("desktop",launcher)
    for component in ("com.android.settings/.Settings","org.lineageos.jelly/.MainActivity",
                      "com.android.calculator2/.Calculator"):
        start(component);time.sleep(4)
    checkpoint("multi","com.android.calculator2")
    if ui_only: return
    start("com.nexon.bluearchive/.MxUnityPlayerActivity");time.sleep(35)
    checkpoint("game35","com.nexon.bluearchive");time.sleep(25)
    checkpoint("game65","com.nexon.bluearchive")

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output",type=Path)
    parser.add_argument("--ui-only",action="store_true",help="Stop after the multi-app checkpoint; do not launch a game")
    args=parser.parse_args()
    main(args.output,args.ui_only)
