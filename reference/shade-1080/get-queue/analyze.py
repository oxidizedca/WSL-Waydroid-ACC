"""Analyze capture.py traces. All durations in the output are wall-clock microseconds."""
from bisect import bisect_right
from collections import Counter, defaultdict
import csv
from pathlib import Path
import re
import sys

def analyze(path, overlaps, slots):
    clients, services, spans = [], [], []
    client, stack, active = {}, [], []
    snapshots = defaultdict(dict)
    timing = None
    prev = None
    mirror_scope = None
    for line in path.read_text().splitlines():
        m = re.search(r'(\S+)-(\d+)\s+\[\d+\].*?\s(\d+)\.(\d{6}): (\w+): (.*)',line)
        if not m: continue
        comm,tid,sec,usec,ev,data = m.groups()
        t = int(sec)*1000000+int(usec)
        kv = {k:int(v,0) for k,v in re.findall(r'(\w+)=(0x[0-9a-f]+|\d+)',data)}
        if ev=="tracing_mark_write":
            timing = data.split()[1:] if data.startswith("motion ") else None
        elif comm=="RenderEngine":
            if ev=="rq":
                client[tid] = dict(start=t, timing=timing, **kv)
            elif ev=="send" and tid in client: client[tid]["send"]=t
            elif ev=="end" and tid in client:
                c=client.pop(tid)
                if c["timing"] and c["phase"]==30 and kv["rc"]==32:
                    clients.append(dict(c,end=t))
        elif comm=="input-broker":
            # Disjoint slices, including the nested GET allowed inside a visibility query.
            if prev is not None and t>prev:
                spans.append((prev,t,stack[-1] if stack else {},active[-1] if active else "dispatch",mirror_scope))
            prev=t
            if ev=="serve":
                stack.append(dict(begin=t))
                active.append("other")
            elif ev=="peek" and stack: stack[-1].update(kv)
            elif ev=="reply" and stack: stack[-1].update(reply=t,rc=kv["phase"],lazy=kv["lazy"])
            elif ev in ("slots_occupied","slots_lazy"):
                snapshots[kv["seq"]][ev]=[kv[f"s{i}"] for i in range(64)]
            elif ev=="served" and stack:
                services.append(stack.pop())
                assert active.pop()=="other"
            elif ev.endswith("_in") and stack:
                active.append(ev[:-3])
                if ev=="mirror_in": mirror_scope={}
            elif ev.endswith("_out") and stack:
                assert active.pop()==ev[:-4]
                if ev=="mirror_out":
                    # uretprobe prints return PC and function PC. ASLR cancels in the difference.
                    ret,entry=map(lambda h:int(h,16),re.findall(r'0x[0-9a-f]+',data)[:2])
                    mirror_scope["kind"]={0x5ec4:"evict",0x5e04:"new",0x598a:"cpu"}[ret-entry+0x4950]
                    mirror_scope=None
    bykey=defaultdict(list)
    for s in services:
        if s.get("phase")==30 and s.get("rc")==32:
            bykey[s["id"],(s["hi"]<<32)|s["lo"],s["serial"]].append(s)
    ends=[s[1] for s in spans]
    groups=Counter()
    seen=set()
    for c in clients:
        key=c["id"],c["nonce"],c["serial"]
        candidates=[s for s in bykey[key] if c["send"]<=s["reply"]<=c["end"]]
        assert len(candidates)==1,(key,c,candidates)
        s=candidates[0]
        start,send,reply,end=[c["start"],c["send"],s["reply"],c["end"]]
        begin=max(send,s["begin"])  # broker can accept a connection before client sends
        row=dict(run=path.parent.name,cycle=c["timing"][0],action=c["timing"][1],
                 first=int(key not in seen),lazy=s["lazy"],total=end-start,setup=send-start,
                 wait=begin-send,service=reply-begin,receive=end-reply)
        seen.add(key)
        i=bisect_right(ends,send)
        weights=Counter()
        while i<len(spans) and spans[i][0]<begin:
            left,right,other,op,scope=spans[i]; i+=1
            if scope: op=scope["kind"]+"."+op
            dt=min(right,begin)-max(left,send)
            if dt<=0: continue
            phase=other.get("phase",-1)
            dims=f'{other.get("w",0)}x{other.get("h",0)}' if phase in (29,36,37) else "-"
            weights[phase,dims,op]+=dt
        if row["wait"]>sum(weights.values()):
            weights[-1,"-","unobserved"]+=row["wait"]-sum(weights.values())
        assert sum(weights.values())==row["wait"]
        for (phase,dims,op),dt in weights.items():
            groups[row["run"],row["first"],row["action"],int(row["total"]>=2000),phase,dims,op]+=dt
        yield row
    assert clients
    overlaps.extend([(*k,v) for k,v in sorted(groups.items())])
    for seq, snap in sorted(snapshots.items()):
        occupied=snap["slots_occupied"]
        lazy=[bool(tex and flag) for tex,flag in zip(occupied,snap["slots_lazy"])]
        idx=(seq-1)%64
        dist=next((i for i in range(64) if not lazy[(idx+i)%64]),64)
        slots.append((path.parent.name,seq,int(lazy[idx]),dist,lazy.count(False),occupied.count(0)))

if __name__=="__main__":
    out=Path(sys.argv[1])
    overlaps,slots=[],[]
    rows=[row for root in sys.argv[2:] for row in analyze(Path(root)/"trace.txt",overlaps,slots)]
    with (out/"requests.csv").open("w",newline="") as f:
        wr=csv.DictWriter(f,rows[0],lineterminator="\n"); wr.writeheader(); wr.writerows(rows)
    with (out/"overlaps.csv").open("w",newline="") as f:
        wr=csv.writer(f,lineterminator="\n")
        wr.writerow(("run","first","action","at_least_2ms","blocking_phase","blocking_size","stage","us"))
        wr.writerows(overlaps)
    if slots:
        with (out/"slots.csv").open("w",newline="") as f:
            wr=csv.writer(f,lineterminator="\n")
            wr.writerow(("run","sequence","victim_lazy","distance_to_cheap","cheap_count","empty_count"))
            wr.writerows(slots)
