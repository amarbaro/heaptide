#!/usr/bin/env python3
# Copyright 2026 amarbaro.org. Licensed under the Apache License, Version 2.0.
# Debug aid for rss_floor: which pages of a traced process's /dev/shm table file are mapped.
import sys,struct
pid=sys.argv[1]
for l in open(f'/proc/{pid}/maps'):
    if '/dev/shm/heaptide' not in l: continue
    a,b=[int(x,16) for x in l.split()[0].split('-')]
    f=open(f'/proc/{pid}/pagemap','rb'); n=0; runs=[]
    step=1<<20
    for s in range(a,b,step):
        f.seek(s//4096*8); d=f.read(min(step,b-s)//4096*8)
        for i in range(len(d)//8):
            if struct.unpack_from('<Q',d,i*8)[0]>>63:
                n+=1; o=(s-a)//4096+i
                if runs and runs[-1][1]==o-1: runs[-1][1]=o
                else: runs.append([o,o])
    print('present pages',n,'=',n*4,'kB'); [print('  pages',r[0],'-',r[1]) for r in runs[:12]]
