"""Windows A/B: measure the adbutils + OpenCV screenshot path, without saving frames."""
import argparse
import csv
import sys
from time import perf_counter
from adbutils import adb
import cv2
import numpy as np

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("output")
    p.add_argument("--stock", default="/system/bin/screencap")
    p.add_argument("--candidate", default="screencap")
    p.add_argument("--stock-label", default="stock")
    p.add_argument("--candidate-label", default="buffered")
    p.add_argument("--device", default="127.0.0.1:15555")
    p.add_argument("--pairs", type=int, default=12)
    args = p.parse_args()
    device = adb.device(args.device)
    rows = []
    for i in range(args.pairs + 1):
        order = [(args.stock_label, args.stock), (args.candidate_label, args.candidate)]
        for name, program in (order if i % 2 else reversed(order)):
            t0 = perf_counter()
            b = device.shell([program, "-p"], stream=False, encoding=None)
            t1 = perf_counter()
            if not b.startswith(b"\x89PNG\r\n\x1a\n") or not b.endswith(b"IEND\xaeB\x60\x82"):
                sys.exit("incomplete PNG: " + name)
            image = cv2.imdecode(np.frombuffer(b, np.uint8), cv2.IMREAD_COLOR)
            if image is None:
                sys.exit("OpenCV could not decode: " + name)
            rgb = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
            t2 = perf_counter()
            if i:
                ms = lambda x: round(1000 * x, 2)
                rows.append([i, name, len(b), rgb.shape[1], rgb.shape[0],
                             ms(t1-t0), ms(t2-t1), ms(t2-t0)])
    with open(args.output, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["pair","variant","bytes","width","height","adb_ms","decode_rgb_ms","total_ms"])
        w.writerows(rows)

if __name__ == "__main__":
    main()
