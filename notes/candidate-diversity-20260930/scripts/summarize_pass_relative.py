#!/usr/bin/env python3
import csv,json,math,random,statistics,sys
from pathlib import Path
D=Path(sys.argv[1]); OLD=D.parent/'extended_2h40'
ARMS=['static25','threat_wide','setup_wide','signals','exchange','combined']

def read(files):
    out={}
    for file in files:
        if not file.exists():continue
        for row in csv.DictReader(file.open()):
            if None in row or any(row.get(key) in (None,'') for key in ['pos','arm','move','chosen','sim_wp','candidates']):continue
            out.setdefault(int(row['pos']),{}).setdefault(row['arm'],{})[row['move']]=row
    return out

def complete(data,pos,arms):
    return all(arm in data.get(pos,{}) and len(data[pos][arm])==int(next(iter(data[pos][arm].values()))['candidates']) and sum(int(row['chosen']) for row in data[pos][arm].values())==1 for arm in arms)

def chosen(records):return next(move for move,row in records.items() if int(row['chosen']))
def stats(values):
    return {'n':len(values),'mean':statistics.mean(values) if values else None,'se':statistics.stdev(values)/math.sqrt(len(values)) if len(values)>1 else None}

def bootstrap(delta,meta,seed=2026092930):
    bygame={}
    for pos,value in delta.items():bygame.setdefault(meta.get(pos,{}).get('game',str(pos)),[]).append(value)
    groups=list(bygame.values());rng=random.Random(seed);means=[]
    for _ in range(3000):
        sample=rng.choices(groups,k=len(groups));means.append(sum(sum(group) for group in sample)/sum(len(group) for group in sample))
    means.sort();return [means[int(.025*len(means))],means[int(.975*len(means))]]

main=read(sorted(D.glob('w[0-7].csv'))+([D/'r0.csv'] if (D/'r0.csv').exists() else [])); old=read(sorted(OLD.glob('w[0-7].csv')))
meta={int(row['pos']):row for row in csv.DictReader((D/'positions.meta.csv').open())}
positions=sorted(pos for pos in main if complete(main,pos,ARMS+['reference']))
checks={}
for file in sorted(D.glob('w[0-7]_checks.csv'))+([D/'r0_checks.csv'] if (D/'r0_checks.csv').exists() else []):
    for row in csv.DictReader(file.open()):checks[int(row['pos']),row['move']]=row
out={'completed_positions':len(positions),'expected_positions':320,'method':json.loads((D/'manifest.json').read_text()),'groups':{},'changed_examples':[],'audit':{}}
blocking_changed=[];reply_errors=[];zero_exchange_errors=[]
for pos in positions:
    reference=main[pos]['reference']
    assert float(next(iter(reference.values()))['wall_ms'])>=59000
    for arm in ARMS:
        assert set(main[pos][arm])<=set(reference)
        for row in main[pos][arm].values():
            assert float(row['wall_ms'])>=14900 or float(row['wall_ms'])<1, (pos,arm,row['wall_ms'])
    if complete(old,pos,ARMS+['reference']) and set(main[pos]['threat_wide'])!=set(old[pos]['threat_wide']):blocking_changed.append(pos)
    for move,row in reference.items():
        check=checks[pos,move]
        assert abs(float(check['blocking_adjustment'])-2*(float(row['threat_value'])-float(row['static_eq'])))<2e-6
        assert abs(float(check['setup_adjustment'])-3*(float(row['setup_value'])-float(row['static_eq'])))<2e-6
        assert float(check['racks'])==64 and int(check['conditioned'])==1
        assert abs(float(check['blocking_delta'])-(float(check['pass_reply_mean'])-float(check['candidate_reply_mean'])))<1e-7
        assert abs(float(check['setup_delta'])-(float(check['candidate_followup_mean'])-float(check['pass_followup_mean'])))<1e-7
        if move in old.get(pos,{}).get('reference',{}):
            legacy=old[pos]['reference'][move]
            error=abs(float(check['candidate_reply_mean'])-(float(legacy['static_eq'])-float(legacy['threat_value']))/.7)
            if error>2e-6:reply_errors.append([pos,move,error])
        if move.startswith('ex ') and (float(check['blocking_delta'])!=0 or float(check['setup_delta'])!=0):zero_exchange_errors.append([pos,move])
        assert all(math.isfinite(float(row[key])) for key in ['sim_wp','sim_eq','sim_wp_sem','sim_eq_sem'])
        assert 0<=float(row['sim_wp'])<=1 and float(row['sim_wp_sem'])>=0
    anchor=main[pos]['static25'];baseline=chosen(anchor)
    for arm in ARMS[1:]:
        move=chosen(main[pos][arm]);ref=reference[move];base=reference[baseline]
        if move!=baseline:
            out['changed_examples'].append({'pos':pos,'arm':arm,'move':move,'rank':int(ref['global_static_rank']),'baseline_move':baseline,'outside_static25':move not in anchor,'wp_gain_pp':100*(float(ref['sim_wp'])-float(base['sim_wp'])),'equity_gain':float(ref['sim_eq'])-float(base['sim_eq']),'sources':[source for source in ARMS[1:-1] if move not in anchor and move in main[pos][source]],'blocking_adjustment':float(checks[pos,move]['blocking_adjustment']),'setup_adjustment':float(checks[pos,move]['setup_adjustment'])})
for group,pp in [('nonopening',[pos for pos in positions if pos<6000]),('opening',[pos for pos in positions if pos>=6000])]:
    results={}
    for arm in ARMS:
        gains={};eqgains=[];widths=[];novel_choices=0;novel_positions=0;cover=[]
        for pos in pp:
            ref=main[pos]['reference'];anchor=main[pos]['static25'];records=main[pos][arm];baseline=chosen(anchor);move=chosen(records)
            gains[pos]=100*(float(ref[move]['sim_wp'])-float(ref[baseline]['sim_wp']))
            eqgains.append(float(ref[move]['sim_eq'])-float(ref[baseline]['sim_eq']));widths.append(len(records));novel_choices+=move not in anchor;novel_positions+=any(move not in anchor for move in records)
            cover.append(100*(max(float(ref[move]['sim_wp']) for move in records)-max(float(ref[move]['sim_wp']) for move in anchor)))
        results[arm]={'candidate_count':stats(widths),'selected_wp_gain_pp':stats(list(gains.values())),'selected_wp_game_bootstrap95':bootstrap(gains,meta) if pp else None,'selected_eq_gain':stats(eqgains),'choices_outside25':novel_choices,'positions_with_additions':novel_positions,'optimistic_reference_coverage_gain_pp':stats(cover)}
    out['groups'][group]=results
out['changed_examples'].sort(key=lambda row:row['wp_gain_pp'],reverse=True)
out['legacy_membership']={}
for arm in ARMS[1:]:
    pp=[pos for pos in positions if complete(old,pos,ARMS+['reference'])]
    out['legacy_membership'][arm]={'positions':len(pp),'changed_pools':sum(set(main[pos][arm])!=set(old[pos][arm]) for pos in pp),'jaccard':stats([len(set(main[pos][arm])&set(old[pos][arm]))/len(set(main[pos][arm])|set(old[pos][arm])) for pos in pp]),'legacy_choices_retained':sum(chosen(old[pos][arm]) in main[pos][arm] for pos in pp)}
out['audit']={'positions_with_blocking_membership_change':blocking_changed,'old_reply_mean_discrepancies':reply_errors,'exchange_nonzero_adjustments':zero_exchange_errors,'all_checks_have_matching_means':True,'all_references_full60s':True,'all_distinct_selection_pools_full15s':True}
# Secondary runs use a separate arm namespace and independent six-ply references.
for name,prefix,arms,refsource,baseline in [
 ('width','v',['static25_fulltime','adaptive_static','static60','adaptive_signals'],main,'static25_fulltime'),
 ('matched','m',['static_setup_count','static_combined_count'],main,None),
 ('sample128','c',['setup_conditioned','signals_conditioned','combined_conditioned'],main,None),
 ('six_ply','d',['static25_fulltime','adaptive_static','static60','adaptive_signals'],None,'static25_fulltime')]:
    data=read(sorted(D.glob(prefix+'[0-7].csv')));full=[pos for pos in data if complete(data,pos,arms+(['reference'] if refsource is None else []))]
    result={}
    for arm in arms:
        gains=[];counts=[];novel=0
        for pos in full:
            ref=(data if refsource is None else refsource)[pos]['reference'];anchor=main[pos]['static25'];basechoice=chosen(data[pos][baseline]) if baseline else chosen(anchor);move=chosen(data[pos][arm]);counts.append(len(data[pos][arm]));novel+=move not in anchor;gains.append(100*(float(ref[move]['sim_wp'])-float(ref[basechoice]['sim_wp'])))
        result[arm]={'candidate_count':stats(counts),'selected_wp_gain_pp':stats(gains),'choices_outside25':novel}
    out[name]={'positions':len(full),'arms':result}
sample_data=read(sorted(D.glob('c[0-7].csv')))
robustness={}
for short,long in [('setup_wide','setup_conditioned'),('signals','signals_conditioned'),('combined','combined_conditioned')]:
    pp=[pos for pos in sample_data if complete(sample_data,pos,['setup_conditioned','signals_conditioned','combined_conditioned']) and pos in positions]
    robustness[short]={'positions':len(pp),'membership_jaccard':stats([len(set(main[pos][short])&set(sample_data[pos][long]))/len(set(main[pos][short])|set(sample_data[pos][long])) for pos in pp]),'changed_choices':sum(chosen(main[pos][short])!=chosen(sample_data[pos][long]) for pos in pp)}
out['sample128']['robustness_vs64']=robustness
quota=read(sorted((D/'tile_followup').glob('w[0-3].csv')))
qarms=['static25_fulltime','tile_quota','tile_exchange'];qp=[pos for pos in quota if complete(quota,pos,qarms+['reference'])];qout={}
for arm in qarms:
    gains=[];count=[];outside=0
    for pos in qp:
        ref=quota[pos]['reference'];anchor=quota[pos]['static25_fulltime'];baseline=chosen(anchor);move=chosen(quota[pos][arm]);gains.append(100*(float(ref[move]['sim_wp'])-float(ref[baseline]['sim_wp'])));count.append(len(quota[pos][arm]));outside+=move not in anchor
    qout[arm]={'candidate_count':stats(count),'selected_wp_gain_pp':stats(gains),'choices_outside25':outside}
out['quota']={'positions':len(qp),'arms':qout}
(D/'summary.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps({key:out[key] for key in ['completed_positions','groups','audit','quota']},indent=2))
