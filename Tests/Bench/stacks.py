#!/usr/bin/env python3
"""Tally of the stack samples of sample.sh: the functions on the most
samples (inclusive), and those at the top of the stacks.
  stacks.py out.txt [function]   only the stacks through function"""
import collections, re, sys

txt = open(sys.argv[1], errors='replace').read()
stacks = re.split(r'\nThread \d+ ', txt)[1:]
incl, top = collections.Counter(), collections.Counter()
for s in stacks:
    fns = []
    for ln in s.split('\n'):
        m = re.match(r'#\d+\s+(?:0x[0-9a-f]+ in )?(\S+)', ln)
        if m:
            fns.append(re.sub(r'\(.*', '', m.group(1)))
    if fns and (len(sys.argv) < 3 or any(sys.argv[2] in f for f in fns)):
        top[fns[0]] += 1
        incl.update(set(fns))
print(f"{len(stacks)} samples\ntop of stack:")
for f, c in top.most_common(12):
    print(f"  {c:4d} {f}")
print("inclusive:")
for f, c in incl.most_common(40):
    print(f"  {c:4d} {f}")
