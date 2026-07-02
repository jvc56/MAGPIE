#!/usr/bin/env python3
"""Head-to-head endgame match: baseline solver vs optimized solver, under a
per-game time bank with a per-turn soft allocation, multi-threaded, at scale.

Each game starts from a bag-empty endgame CGP. The two engines alternate moves
(optimized plays one colour, baseline the other) via per-move subprocess calls
to `egmove1`, each solving under a soft/hard time control drawn from that
player's remaining game bank. Colours are swapped every other game so the mean
of opt's per-game margin isolates opt's *strength* edge (identical play -> 0 by
symmetry; a deeper/better move -> positive). Runs until a wall-clock deadline.

Fully interrupt-safe: every finished game is appended to games.jsonl and fsync'd
immediately, and summary.txt is rewritten after every game, so a kill at any
point leaves complete, detailed results for all games played so far.

Env: EGM_THREADS (18) EGM_BANK (5.0 s/player/game) EGM_HOURS (8.0)
     EGM_OUT (/tmp/egmatch) EGM_BATTERY (/tmp/nonstuck_cgps.txt) EGM_LEX (CSW21)
"""
import os, sys, time, json, subprocess, signal, statistics

OURS   = os.environ.get("EGM_OURS", "/tmp/eg_ours_match")
BASE   = os.environ.get("EGM_BASE", "/tmp/eg_base_match")
BATTERY= os.environ.get("EGM_BATTERY", "/tmp/nonstuck_cgps.txt")
LEX    = os.environ.get("EGM_LEX", "CSW21")
THREADS= int(os.environ.get("EGM_THREADS", "18"))
BANK   = float(os.environ.get("EGM_BANK", "5.0"))
HOURS  = float(os.environ.get("EGM_HOURS", "8.0"))
OUT    = os.environ.get("EGM_OUT", "/tmp/egmatch")
MOVE_CAP = 40
os.makedirs(OUT, exist_ok=True)
JSONL  = os.path.join(OUT, "games.jsonl")
SUMMARY= os.path.join(OUT, "summary.txt")

start = time.time()
deadline = start + HOURS*3600.0
_stop = {"flag": False}
def _sig(_s,_f): _stop["flag"]=True
signal.signal(signal.SIGINT, _sig); signal.signal(signal.SIGTERM, _sig)

def solve_move(binary, cgp, bank):
    env = dict(os.environ)
    env["MAGPIE_M1_CGP"]=cgp
    env["MAGPIE_M1_HARD"]=f"{max(bank,0.05):.4f}"
    env["MAGPIE_M1_THREADS"]=str(THREADS)
    env["MAGPIE_M1_LEX"]=LEX
    try:
        p = subprocess.run([binary,"egmove1"], env=env, capture_output=True,
                           text=True, timeout=max(bank,0.05)+120)
    except subprocess.TimeoutExpired:
        return None
    for ln in p.stdout.splitlines():
        if ln.startswith("M1 s0="):
            body,_,cgpv = ln.partition(" cgp=")
            d={}
            for tok in body.split()[1:]:
                k,v=tok.split("=",1); d[k]=v
            d["cgp"]=cgpv
            return d
    return None

def play_game(pos_idx, pos_cgp, opt_color):
    cgp = pos_cgp
    banks = [BANK, BANK]          # per player-index bank (seconds)
    on = 0                        # game_load_cgp puts player 0 on turn
    moves=[]
    ended=0; s0=s1=0
    for k in range(MOVE_CAP):
        engine = OURS if on==opt_color else BASE
        who = "opt" if on==opt_color else "base"
        r = solve_move(engine, cgp, banks[on])
        if r is None:
            moves.append({"error":"solve_failed","on":on,"who":who}); break
        used=float(r["used"]); banks[on]-=used
        s0=int(r["s0"]); s1=int(r["s1"]); ended=int(r["ended"])
        moves.append({"on":on,"who":who,"used":used,"soft":float(r["soft"]),
                      "depth":int(r["depth"]),"nodes":int(r["nodes"]),
                      "tiny":int(r["tiny"]),"s0":s0,"s1":s1,"bank_left":round(banks[on],4)})
        cgp=r["cgp"]
        if ended: break
        nxt=int(r["onmove"])
        on = nxt
    spread = s0 - s1
    opt_margin = spread if opt_color==0 else -spread
    return {"pos":pos_idx,"opt_color":opt_color,"nmoves":len(moves),
            "ended":ended,"s0":s0,"s1":s1,"spread":spread,
            "opt_margin":opt_margin,
            "opt_time":round(sum(m.get("used",0) for m in moves if m.get("who")=="opt"),3),
            "base_time":round(sum(m.get("used",0) for m in moves if m.get("who")=="base"),3),
            "moves":moves}

def write_summary(recs, ngames, npos, last):
    # recs: list of (opt_color, opt_margin, spread) in play order; consecutive
    # (c0,c1) entries are the same position played both ways -- pair them so the
    # (large) position value cancels and only genuine divergence in PLAY shows.
    el=time.time()-start
    pairs=[]
    for k in range(0, len(recs)-1, 2):
        (c0,m0,s0),(c1,m1,s1)=recs[k],recs[k+1]
        if c0==0 and c1==1:
            pairs.append(((m0+m1)/2.0, s0!=s1, s0-s1))  # (opt advantage, diverged?, spread gap)
    with open(SUMMARY,"w") as f:
        f.write("endgame head-to-head: optimized vs baseline\n")
        f.write(f"  threads={THREADS} bank={BANK}s/player/game lex={LEX}\n")
        f.write(f"  elapsed={el/3600:.2f}h  games={ngames}  positions_cycled={npos}\n")
        if pairs:
            adv=[p[0] for p in pairs]
            div=[p for p in pairs if p[1]]
            mean=statistics.mean(adv)
            sd=statistics.pstdev(adv) if len(adv)>1 else 0.0
            se=sd/(len(adv)**0.5)
            f.write(f"  colour-paired positions: {len(pairs)}\n")
            f.write(f"  --> OPT NET STRENGTH: mean {mean:+.4f} pts/game  (95% CI +-{1.96*se:.4f})\n")
            f.write(f"      stdev {sd:.3f}\n")
            f.write(f"  divergent positions (play actually differed): {len(div)}/{len(pairs)} ({100*len(div)/len(pairs):.3f}%)\n")
            if div:
                dv=[d[0] for d in div]
                f.write(f"      among divergent: mean opt advantage {statistics.mean(dv):+.3f} pts, "
                        f"opt better {sum(1 for d in div if d[0]>0)}, worse {sum(1 for d in div if d[0]<0)}, net {sum(dv):+.1f}\n")
        # also the naive per-game margin mean (should track 0 once colour-balanced)
        m=[r[1] for r in recs]
        if m:
            f.write(f"  (raw per-game margin mean over {len(m)} games: {statistics.mean(m):+.3f})\n")
        f.write(f"  last: {last}\n")

def main():
    cgps=[l.strip() for l in open(BATTERY) if l.strip()]
    # fresh run: truncate outputs
    open(JSONL,"w").close()
    recs=[]; ngames=0; gi=0
    fh=open(JSONL,"a")
    npos=0
    i=0
    while not _stop["flag"] and time.time()<deadline:
        pos_idx=i%len(cgps); pos=cgps[pos_idx]
        if pos_idx==0: npos+=1
        for opt_color in (0,1):
            if _stop["flag"] or time.time()>=deadline: break
            g=play_game(pos_idx,pos,opt_color)
            g["game"]=gi; g["t"]=round(time.time()-start,1); gi+=1; ngames+=1
            recs.append((opt_color, g["opt_margin"], g["spread"]))
            fh.write(json.dumps(g)+"\n"); fh.flush(); os.fsync(fh.fileno())
            if ngames%10==0 or ngames<20:
                write_summary(recs,ngames,npos,
                              f"pos{pos_idx} c{opt_color} margin={g['opt_margin']:+d} nmoves={g['nmoves']}")
        i+=1
    write_summary(recs,ngames,npos,"DONE" if not _stop["flag"] else "INTERRUPTED")
    fh.close()
    print(f"finished: {ngames} games written to {JSONL}")

if __name__=="__main__":
    main()
