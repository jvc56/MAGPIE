#!/usr/bin/env python3
import csv,hashlib,html,json,os,subprocess,time
from pathlib import Path
D=Path('/Users/olaugh/sources/magpie-pat-postx/postx/threatgames/pass_relative_20260929'); OLD=D.parent/'extended_2h40';REPO=Path('/Users/olaugh/sources/magpie-pat-pass-relative');BIN=D/'magpie_test_pass_relative'
(D/'finalizer_pid.json').write_text(json.dumps({'pid':os.getpid()}))
(D/'finalizer_stage.json').write_text(json.dumps({'stage':'waiting_base_suite'}))

def load(files):
    data={}
    for file in files:
        for row in csv.DictReader(file.open()):data.setdefault(int(row['pos']),{}).setdefault(row['arm'],{})[row['move']]=row
    return data

def chosen(records):return next(move for move,row in records.items() if int(row['chosen']))

def summary():subprocess.run(['python3',str(D/'summarize_pass_relative.py'),str(D)],check=True,stdout=(D/'summary_console.json').open('w'))
while not (D/'base_suite_complete.json').exists() or not (D/'terminal_repair_complete.json').exists():
    time.sleep(20)
summary();s=json.loads((D/'summary.json').read_text());assert s['completed_positions']==320 and s['quota']['positions']==40
assert s['width']['positions']==128 and s['matched']['positions']==128 and s['sample128']['positions']==128 and s['six_ply']['positions']==64
main=load(list(D.glob('w[0-7].csv'))+([D/'r0.csv'] if (D/'r0.csv').exists() else []));quota=load((D/'tile_followup').glob('w[0-3].csv'))
targets=[pos for pos in [1217,493,580,1100,2241,1755] if pos in main]
novel=[row for row in s['changed_examples'] if row['outside_static25'] and row['arm'] in ['setup_wide','combined']]
for row in novel[:3]+list(reversed(novel[-3:])):
    if row['pos'] not in targets:targets.append(row['pos'])
for arm in ['threat_wide','exchange','signals']:
    arm_examples=[row for row in s['changed_examples'] if row['outside_static25'] and row['arm']==arm]
    for row in arm_examples[:1]+arm_examples[-1:]:
        if row['pos'] not in targets:targets.append(row['pos'])
for row in s['changed_examples']:
    if row['outside_static25'] and row['pos'] not in targets:targets.append(row['pos'])
positions={int(line.split(',',1)[0]):line for line in (D/'positions.cgp').read_text().splitlines()}
qpositions={int(line.split(',',1)[0]):line for line in (D/'tile_followup'/'positions.cgp').read_text().splitlines()}
(D/'finalizer_stage.json').write_text(json.dumps({'stage':'preparing_deep_references'}))
requests=[]
for data,name,ids,lines in [(main,'primary',targets,positions),(quota,'quota',[106,6007],qpositions)]:
    for pos in ids:
        records=data[pos];anchor=records['static25' if name=='primary' else 'static25_fulltime'];moves=set(anchor)
        for arm in (['threat_wide','setup_wide','signals','exchange','combined'] if name=='primary' else ['tile_quota','tile_exchange']):moves.add(chosen(records[arm]))
        old_directory=OLD if name=='primary' else OLD/'tile_followup'
        old_deep=old_directory/(f'new{pos}_all25_p16.csv' if name=='primary' else f'validate{pos}_p16.csv')
        if old_deep.exists():
            moves.update(row['move'] for row in csv.DictReader(old_deep.open()) if row['move'] in records['reference'])
        ranks=','.join(str(records['reference'][move]['static_rank']) for move in sorted(moves,key=lambda move:int(records['reference'][move]['static_rank'])))
        directory=D if name=='primary' else D/'tile_followup'
        inputfile=directory/f'new{pos}.cgp';inputfile.write_text(lines[pos]+'\n')
        requests.append((name,pos,directory,inputfile,ranks))
(D/'deep_validation_manifest.json').write_text(json.dumps({'targets':[{'dataset':name,'pos':pos,'ranks':ranks} for name,pos,directory,inputfile,ranks in requests],'reference_ms':600000,'plies':16,'reference_seed':2026092941,'admission_ms':15000,'rollout':'static noPAT','selection':'original top25 plus all selected plays and previously validated additions; includes every primary position with a choice outside static25 and the two prior quota examples'},indent=2))
common={'PCD_PASS_RELATIVE':'1','PCD_ADAPTIVE':'1','PCD_POOL':'120','PCD_EXCHANGE_QUOTA':'5','PCD_EXCHANGE_MARGIN':'35','PCD_CONDITION_DRAWS':'1','PCD_SELECTION_NO_CUTOFF':'1','PCD_PLIES':'16','PCD_MS':'15000','PCD_SEED':'2026092941'}

def run_request(request):
    name,pos,directory,inputfile,ranks=request
    filename=f'new{pos}_all25_p16.csv' if name=='primary' else f'validate{pos}_p16.csv'
    env=os.environ|common|{'PCD_IN':str(inputfile),'PCD_OUT':str(directory/filename),'PCD_VALIDATE_RANKS':ranks,'PCD_REF_MS':'600000'}
    log=(directory/(filename+'.log')).open('a');proc=subprocess.Popen([str(BIN),'patcanddiversity'],cwd=REPO,env=env,stdout=log,stderr=subprocess.STDOUT);log.close();return proc
for offset in range(0,len(requests),8):
    (D/'finalizer_stage.json').write_text(json.dumps({'stage':'deep_references','wave':offset//8+1,'targets':len(requests)}))
    wave=requests[offset:offset+8];processes=[run_request(request) for request in wave]
    for proc in processes:assert proc.wait()==0
(D/'finalizer_stage.json').write_text(json.dumps({'stage':'deep_admission'}))
# Timed tests measure whether admitting those moves still helps at a deep rollout.
for offset in range(0,len(requests),8):
    procs=[]
    (D/'finalizer_stage.json').write_text(json.dumps({'stage':'deep_admission','wave':offset//8+1,'targets':len(requests)}))
    for name,pos,directory,inputfile,ranks in requests[offset:offset+8]:
        env=os.environ|common|{'PCD_IN':str(inputfile),'PCD_OUT':str(directory/f'admission{pos}_p16.csv'),'PCD_ADMIT_RANKS':ranks}
        log=(directory/f'admission{pos}_p16.log').open('a');procs.append(subprocess.Popen([str(BIN),'patcanddiversity'],cwd=REPO,env=env,stdout=log,stderr=subprocess.STDOUT));log.close()
    for proc in procs:assert proc.wait()==0
summary();s=json.loads((D/'summary.json').read_text())
s['deep16']=[]
for name,pos,directory,inputfile,ranks in requests:
    filename=f'new{pos}_all25_p16.csv' if name=='primary' else f'validate{pos}_p16.csv'
    data=load([directory/filename])[pos]['reference'];primary=(main if name=='primary' else quota)[pos];anchor=primary['static25' if name=='primary' else 'static25_fulltime'];best25=max(anchor,key=lambda move:float(data[move]['sim_wp']));base=chosen(anchor)
    assert len(data)==len(ranks.split(',')) and float(next(iter(data.values()))['wall_ms'])>=599000
    for move,row in data.items():
        if move in anchor:continue
        s['deep16'].append({'dataset':name,'pos':pos,'move':move,'rank':int(row['global_static_rank']),'wp_gain_vs_best25_pp':100*(float(row['sim_wp'])-float(data[best25]['sim_wp'])),'wp_gain_vs_selected25_pp':100*(float(row['sim_wp'])-float(data[base]['sim_wp'])),'sem_sum_upper_pp':100*(float(row['sim_wp_sem'])+float(data[best25]['sim_wp_sem'])),'best25_move':best25,'chosen25_move':base})
(D/'summary.json').write_text(json.dumps(s,indent=2)+'\n')
(D/'finalizer_stage.json').write_text(json.dumps({'stage':'auditing_and_building_page'}))
# Verify candidate identity and all records before publishing the new page.
debug=D/'debug';debug.mkdir(exist_ok=True)
env=os.environ|{'CANDIDATE_DEBUG_SOURCE':str(D),'CANDIDATE_DEBUG_OUTPUT':str(debug/'candidate-pools-debug.html')}
subprocess.run(['python3',str(D/'build_candidate_debug.py')],env=env,check=True,stdout=(D/'debug_build.log').open('w'))
# Keep the familiar local preview URL, and retain the original page separately.
legacy=OLD/'debug'/'candidate-pools-debug.html';legacy_backup=OLD/'debug'/'candidate-pools-debug-legacy.html'
if not legacy_backup.exists():legacy_backup.write_bytes(legacy.read_bytes())
legacy.write_bytes((debug/'candidate-pools-debug.html').read_bytes())
files=list(D.glob('*.csv'))+list((D/'tile_followup').glob('*.csv'))+list(D.glob('*.cgp'))+[D/'harness_snapshot.c',BIN]
(D/'final_hashes.json').write_text(json.dumps({str(file.relative_to(D)):hashlib.sha256(file.read_bytes()).hexdigest() for file in files},indent=2))
rows=[]
for group,arms in s['groups'].items():
    for arm,result in arms.items():
        mean=result['selected_wp_gain_pp']['mean'];se=result['selected_wp_gain_pp']['se'];ci=result['selected_wp_game_bootstrap95'];rows.append(f'<tr><td>{group}</td><td>{arm}</td><td>{result["candidate_count"]["mean"]:.2f}</td><td>{mean:+.4f} ± {se:.4f}</td><td>{ci[0]:+.4f} to {ci[1]:+.4f}</td><td>{result["choices_outside25"]}</td></tr>')
report='<!doctype html><html><meta charset="utf-8"><title>Pass-relative candidate diversity</title><style>body{font:15px system-ui;margin:40px;max-width:1100px;color:#263b31}table{border-collapse:collapse;width:100%}td,th{padding:9px;border-bottom:1px solid #ddd;text-align:left}pre{white-space:pre-wrap;background:#f3f5ef;padding:18px}</style><h1>Pass-relative candidate diversity rerun</h1><p>320 primary positions, 128 width/sample-size controls, 64 six-ply positions, 40 fresh quota positions, and targeted independent 16-ply validations. All rollouts use no-PAT static play. Every distinct selection pool receives 15 seconds; independent round-robin references receive 60 seconds.</p><p>Blocking = 1.4 × (opponent reply after pass − opponent reply after candidate). Setup = 0.75 × (our next-turn opportunity after candidate/reply − after pass/reply), using the same sampled opponent rack and matched candidate leave/refill. Positive adjustments indicate reduced opponent scoring or improved next-turn scoring. Refills exclude the opponent rack.</p><p>Results below compare each policy’s selected play with the selected static25 play, evaluated in the same independent reference. The game-cluster bootstrap includes zero-change positions. These are sampled sim estimates, not a full-game strength test. Positions reuse the old study for a paired policy comparison.</p><table><tr><th>Phase</th><th>Pool</th><th>Mean candidates</th><th>Win gain (pp) ± position SE</th><th>95% game bootstrap</th><th>Chosen outside25</th></tr>'+''.join(rows)+'</table><h2>Integrity checks</h2><pre>'+html.escape(json.dumps(s['audit'],indent=2))+'</pre><h2>Secondary studies and deep validation</h2><pre>'+html.escape(json.dumps({key:s[key] for key in ['width','matched','sample128','six_ply','quota','deep16']},indent=2))+'</pre></html>'
(D/'REPORT.html').write_text(report)
(D/'finalizer_stage.json').write_text(json.dumps({'stage':'complete'}))
(D/'complete.json').write_text(json.dumps({'completed_epoch':time.time(),'primary_positions':320,'quota_positions':40,'deep_requests':len(requests),'page':str(debug/'candidate-pools-debug.html'),'report':str(D/'REPORT.html')},indent=2))
