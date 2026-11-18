import re, sys
src, out, mode_a, mode_b = sys.argv[1:5]
pos={};picks={}
for l in open(src):
    m=re.match(r"SIMX pos idx=(\d+) game=\d+ bag=\d+ (cgp .*)",l)
    if m: pos[int(m[1])]=m[2].strip(); continue
    m=re.match(r"SIMX pick pos=(\d+) n=(\d+) mode=(\w+) move=(.*?) iters=",l)
    if m: picks.setdefault(int(m[1]),{})[m[3]]=m[4].strip()
n=0
with open(out,"w") as f:
    for i in sorted(picks):
        p=picks[i]
        if mode_a in p and mode_b in p and p[mode_a]!=p[mode_b]:
            f.write(f"{i}\t{pos[i]}\t{p[mode_a]}\t{p[mode_b]}\n"); n+=1
print("differing", n, "of", len(picks))
