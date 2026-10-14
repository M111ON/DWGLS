#!/usr/bin/env python3
"""Parse the benchmark logs into res_geo.json / res_ann.json / res_pq.json."""
import json, re, os

OUT = "I:/tmp/ann_eval"

def parse_ann(path):
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"(\S+)\s+build=\s*([\d.]+)s\s+mem=\s*([\d.]+)MB\s+lat=\s*([\d.]+)ms\s+R@1=([\d.]+)\s+R@10=([\d.]+)\s+frac=(\S+)", line)
        if m:
            rows.append(dict(method=m.group(1), build_s=float(m.group(2)), mem_mb=float(m.group(3)),
                             lat_ms=float(m.group(4)), r1=float(m.group(5)), r10=float(m.group(6)),
                             frac=(None if m.group(7)=="nan" else float(m.group(7)))))
    return rows

ann = parse_ann(f"{OUT}/bench_ann.log")
json.dump(ann, open(f"{OUT}/res_ann.json","w"), indent=1)
print("res_ann.json:", len(ann), "rows")

# PQ (only m48 completed before kill)
pq = [dict(method="PQ_m48_b8", build_s=141.9, mem_mb=2.9, lat_ms=4.325, r1=0.58, r10=0.4183, note="adc-exhaustive")]
json.dump(pq, open(f"{OUT}/res_pq.json","w"), indent=1)
print("res_pq.json:", len(pq), "rows")

# GEO results captured from bench_geo.py stdout (transcribed)
geo = [
 ("quad_PC01_g8_w1",  0.953,0.892,0.3232),("quad_PC01_g8_w2",0.993,0.977,0.5095),("quad_PC01_g8_w3",0.993,0.995,0.6619),
 ("quad_PC01_g16_w1", 0.787,0.628,0.1343),("quad_PC01_g16_w2",0.927,0.850,0.2681),("quad_PC01_g16_w3",0.983,0.935,0.3780),
 ("quad_PC01_g32_w1", 0.563,0.322,0.0412),("quad_PC01_g32_w2",0.747,0.546,0.1014),("quad_PC01_g32_w3",0.850,0.715,0.1704),
 ("quad_PC01_g64_w1", 0.380,0.150,0.0111),("quad_PC01_g64_w2",0.510,0.267,0.0297),("quad_PC01_g64_w3",0.630,0.384,0.0553),
 ("quad_PC01_g128_w1",0.193,0.059,0.0029),("quad_PC01_g128_w2",0.333,0.123,0.0078),("quad_PC01_g128_w3",0.410,0.179,0.0151),
 ("quad_PC01_g256_w1",0.067,0.020,0.0007),("quad_PC01_g256_w2",0.147,0.044,0.0020),("quad_PC01_g256_w3",0.230,0.074,0.0039),
 ("oct_union_g16_w1", 0.927,0.911,0.4343),("oct_inter_g16_w1",0.460,0.167,0.0010),
 ("oct_union_g16_w2", 0.993,0.991,0.7724),("oct_inter_g16_w2",0.697,0.514,0.0156),
 ("oct_union_g32_w1", 0.697,0.573,0.1414),("oct_inter_g32_w1",0.333,0.072,0.0000),
 ("oct_union_g32_w2", 0.893,0.852,0.3342),("oct_inter_g32_w2",0.433,0.126,0.0004),
 ("oct_union_g64_w1", 0.507,0.265,0.0385),("oct_inter_g64_w1",0.210,0.043,0.0000),
]
json.dump([dict(method=a,r1=b,r10=c,frac=d) for a,b,c,d in geo], open(f"{OUT}/res_geo.json","w"), indent=1)
print("res_geo.json:", len(geo), "rows")
