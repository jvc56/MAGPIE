#!/usr/bin/env python3
import csv,hashlib,json,os,subprocess,time
from pathlib import Path
D=Path('/Users/olaugh/sources/magpie-pat-postx/postx/threatgames/pass_relative_20260929')
OLD=D.parent/'extended_2h40'
REPO=Path('/Users/olaugh/sources/magpie-pat-pass-relative')
BIN=D/'magpie_test_pass_relative'
COMMON={'PCD_PASS_RELATIVE':'1','PCD_ADAPTIVE':'1','PCD_EXPLORE':'1','PCD_POOL':'120','PCD_EXCHANGE_QUOTA':'5','PCD_EXCHANGE_MARGIN':'35','PCD_CONDITION_DRAWS':'1','PCD_MS':'15000','PCD_REF_MS':'60000','PCD_PLIES':'4','PCD_SELECTION_NO_CUTOFF':'1'}

def run_group(label,prefix,workers,positions,extra):
    processes=[]
    for worker in range(workers):
        env=os.environ|COMMON|extra|{'PCD_IN':str(positions),'PCD_OUT':str(D/f'{prefix}{worker}.csv'),'PCD_CHECK_OUT':str(D/f'{prefix}{worker}_checks.csv'),'PCD_WORKER':str(worker),'PCD_NUM_WORKERS':str(workers)}
        log=(D/f'{prefix}{worker}.log').open('a')
        proc=subprocess.Popen([str(BIN),'patcanddiversity'],cwd=REPO,env=env,stdout=log,stderr=subprocess.STDOUT)
        log.close();processes.append(proc)
    (D/f'{label}_pids.json').write_text(json.dumps([proc.pid for proc in processes]))
    return label,processes

def wait_groups(groups):
    for label,processes in groups:
        codes=[proc.wait() for proc in processes]
        (D/f'{label}_status.json').write_text(json.dumps({'finished':time.time(),'exit_codes':codes}))
        if any(codes):raise RuntimeError(f'{label}: {codes}')

manifest={'started_epoch':time.time(),'source':'same 320 primary positions and 40 independent quota positions as prior study; new sims and pass-relative checks','blocking':'1.4 * mean(pass opponent best placement score - candidate opponent best placement score), same sampled racks','setup':'0.75 * mean(our best placement after candidate and opponent reply - our best placement after pass and opponent reply), same candidate leave/refill in both branches','conditioned_draws':True,'racks':64,'selection_ms':15000,'selection_cutoff':'disabled; full 15-second runs','reference_ms':60000,'rollout':'static noPAT','plies':4,'workers':8,'arms':['static25','threat_wide','setup_wide','signals','exchange','combined'],'binary_sha256':hashlib.sha256(BIN.read_bytes()).hexdigest(),'old_results_retained':str(OLD),'secondary':{'width':128,'matched_width':128,'six_ply':64,'rack_sample_robustness':128,'quota':40}}
(D/'manifest.json').write_text(json.dumps(manifest,indent=2))
wait_groups([run_group('primary','w',8,D/'positions.cgp',{})])
# Matched-width counts come from the newly measured pools.
counts={}
for f in D.glob('w[0-7].csv'):
    for row in csv.DictReader(f.open()):
        if row['arm'] in ['setup_wide','combined']:counts[int(row['pos']),row['arm']]=int(row['candidates'])
with (D/'matched_counts.csv').open('w') as out:
    out.write('pos,setup_count,combined_count\n')
    for line in (D/'conditioned_positions.cgp').read_text().splitlines():
        pos=int(line.split(',',1)[0]);out.write(f"{pos},{counts[pos,'setup_wide']},{counts[pos,'combined']}\n")
wait_groups([run_group('width','v',4,D/'conditioned_positions.cgp',{'PCD_WIDTH_ONLY':'1'}),run_group('depth','d',4,D/'depth_positions.cgp',{'PCD_WIDTH_ONLY':'1','PCD_WIDTH_REFERENCE':'1','PCD_PLIES':'6'})])
wait_groups([run_group('matched','m',4,D/'conditioned_positions.cgp',{'PCD_MATCHED_ONLY':'1','PCD_MATCHED_COUNTS':str(D/'matched_counts.csv'),'PCD_SELECTION_NO_CUTOFF':'1'}),run_group('checks128','c',4,D/'conditioned_positions.cgp',{'PCD_CONDITION_ONLY':'1','PCD_CHECK_R':'128'})])
quota=D/'tile_followup'
quota.mkdir(exist_ok=True)
for name in ['positions.cgp','positions.meta.csv','candidate_features.csv']:(quota/name).write_bytes((OLD/'tile_followup'/name).read_bytes())
# Quota workers share the same run function with explicit output paths.
procs=[]
for worker in range(4):
    env=os.environ|COMMON|{'PCD_TILE_ONLY':'1','PCD_TILE_REFERENCE':'1','PCD_SELECTION_NO_CUTOFF':'1','PCD_SEED':'2026092983','PCD_IN':str(quota/'positions.cgp'),'PCD_OUT':str(quota/f'w{worker}.csv'),'PCD_CHECK_OUT':str(quota/f'w{worker}_checks.csv'),'PCD_WORKER':str(worker),'PCD_NUM_WORKERS':'4'}
    log=(quota/f'w{worker}.log').open('a');procs.append(subprocess.Popen([str(BIN),'patcanddiversity'],cwd=REPO,env=env,stdout=log,stderr=subprocess.STDOUT));log.close()
wait_groups([('quota',procs)])
(D/'base_suite_complete.json').write_text(json.dumps({'finished_epoch':time.time(),'complete':True}))
