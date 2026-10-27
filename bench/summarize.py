#!/usr/bin/env python3
"""Summarize actual benchmark CSV; standard library only."""
import csv
import statistics
import sys
from collections import defaultdict
path = sys.argv[1] if len(sys.argv) > 1 else 'docs/benchmark-raw.csv'
groups = defaultdict(list)
with open(path, newline='') as f:
    for row in csv.DictReader(f):
        groups[(row['query'], row['mode'])].append(row)
print('| Query | Row store ms | Column ms, no zones | Column ms, zones | Speedup vs row | Bytes, zones | Groups skipped |')
print('|---|---:|---:|---:|---:|---:|---:|')
for query in dict.fromkeys(key[0] for key in groups):
    modes = ['row_store', 'column_no_zones', 'column_zones']
    med = [statistics.median(float(r['elapsed_ms']) for r in groups[query, mode]) for mode in modes]
    r = groups[query, 'column_zones'][0]
    print(f"| {query} | {med[0]:.3f} | {med[1]:.3f} | {med[2]:.3f} | {med[0]/med[2]:.1f}x | {int(r['bytes_read']):,} | {r['groups_skipped']} |")
