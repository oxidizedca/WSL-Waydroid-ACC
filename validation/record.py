"""Windows Graphics Capture. Run after the timing batches."""
import subprocess as sp
import sys
import time
from pathlib import Path

mode=sys.argv[1]
assert mode in ("101","111")
out=Path(__file__).resolve().parent/(mode+"-60fps.mp4")
host=["wsl","-u","root","-e"]
android=[*host,"lxc-attach","-P","/var/lib/waydroid/lxc","-n","waydroid","--"]
def act(*args):
    return sp.check_output([*android,*args],text=True).strip()
mark="/run/waydroid-acc-query-on"
original=sp.run([*host,"test","-e",mark]).returncode==0
old={k:act("getprop","debug.waydroid.acc_"+k) for k in ["bind","pool","trace"]}
try:
    for key,val in [("bind","1"),("pool","1"),("trace","0")]:
        act("setprop","debug.waydroid.acc_"+key,val)
    if mode=="111": sp.run([*host,"touch",mark],check=True)
    elif original: sp.run([*host,"unlink",mark],check=True)
    act("cmd","statusbar","collapse")
    act("am","start","-a","android.settings.SETTINGS")
    time.sleep(.8)
    with sp.Popen(["ffmpeg","-nostdin","-v","error","-y","-stats_period","0.15","-progress","pipe:1","-f","lavfi","-i",
            "gfxcapture=window_title=^Weston.Compositor:max_framerate=60:capture_cursor=0:width=960:height=540:resize_mode=scale",
            "-vf","hwdownload,format=bgra,format=yuv420p","-r","60","-fps_mode","cfr","-frames:v","720",
            "-c:v","libx264","-preset","veryfast","-crf","16",str(out)],stdout=sp.PIPE,text=True) as recorder:
        for line in recorder.stdout:
            if line.startswith("frame=") and int(line.split("=")[1])>0: break
        act("input","swipe","320","820","320","280","380")
        time.sleep(.45)
        act("input","swipe","320","280","320","820","380")
        time.sleep(.5)
        act("am","start","-n","com.android.calculator2/.Calculator")
        time.sleep(.65)
        act("cmd","statusbar","expand-notifications")
        time.sleep(.9)
        act("cmd","statusbar","collapse")
        time.sleep(.7)
        act("input","keyevent","KEYCODE_APP_SWITCH")
        time.sleep(.9)
        act("input","keyevent","KEYCODE_HOME")
        _,_=recorder.communicate(timeout=25)
        assert recorder.returncode==0
finally:
    for key,val in old.items(): act("setprop","debug.waydroid.acc_"+key,val)
    if original: sp.run([*host,"touch",mark],check=True)
    else: sp.run([*host,"rm","-f",mark],check=True)
