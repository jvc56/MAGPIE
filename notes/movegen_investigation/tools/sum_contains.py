import sys, re
tot = {}
for line in sys.stdin:
    m = re.match(r'\s*([\d.]+)%\s+(.*)$', line)
    if not m: continue
    for n in m.group(2).split(' + '):
        n = n.strip(); tot[n] = tot.get(n, 0) + float(m.group(1))
for k, v in sorted(tot.items(), key=lambda x: -x[1]): print(f'{v:7.2f}%  {k}')
