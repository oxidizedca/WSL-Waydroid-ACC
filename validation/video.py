"""Windows-side scan of the Android content and contiguous dark rows in every decoded frame."""
from pathlib import Path
import csv
import subprocess as sp
import sys
import numpy as np
from PIL import Image,ImageDraw

path=Path(sys.argv[1])
raw=sp.check_output(["ffmpeg","-loglevel","error","-i",str(path),"-vf","scale=320:180",
                     "-pix_fmt","rgb24","-f","rawvideo","pipe:1"])
frames=np.frombuffer(raw,np.uint8).reshape(-1,180,320,3)
assert len(frames)==720
# Exclude Weston chrome, client edges and Android status/navigation bars.
roi=frames[:,18:158,6:309].astype(np.int16)
black=(roi.max(axis=3)<18)
dark=black.mean(axis=(1,2))
bands=(black.mean(axis=2)>.95).sum(axis=1)/black.shape[1]
changed=np.r_[0,np.abs(np.diff(roi,axis=0)).mean(axis=(1,2,3))]
with path.with_suffix(".csv").open("w",newline="") as f:
    wr=csv.writer(f);wr.writerow(["frame","near_black_fraction","full_width_dark_row_fraction","mean_channel_delta"])
    for i in range(len(frames)): wr.writerow([i,round(dark[i],5),round(bands[i],5),round(changed[i],3)])
indices=sorted(set(range(0,720,45))|{int(dark.argmax()),int(bands.argmax()),int(changed.argmax()),719})
sheet=Image.new("RGB",(5*200,((len(indices)+4)//5)*130),"white");draw=ImageDraw.Draw(sheet)
for j,i in enumerate(indices):
    x=j%5*200;y=j//5*130
    sheet.paste(Image.fromarray(frames[i]).resize((200,112)),(x,y))
    draw.text((x+4,y+113),f"#{i}  {i/60:.2f} s",fill="black")
sheet.save(path.with_suffix(".jpg"),quality=91)
print("frames",len(frames),"peak_dark",round(dark.max(),4),"peak_bands",round(bands.max(),4),"changed",sum(changed>0.12))
