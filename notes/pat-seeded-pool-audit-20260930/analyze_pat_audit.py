import csv,json,pathlib,tarfile,io,collections,statistics
root=pathlib.Path('/Users/olaugh/sources/magpie-pat-simcmp');out=root/'notes/pat-seeded-pool-audit-20260930'
summary={};details=[]
for dataset,offset in [('overnight',0),('blocking',2000)]:
 feats=collections.defaultdict(list);checks=collections.defaultdict(dict);old=collections.defaultdict(lambda:collections.defaultdict(set));chosen=collections.defaultdict(dict)
 for r in csv.DictReader(open(out/f'{dataset}-candidate_features.csv')): feats[int(r['pos'])].append(r)
 for p in out.glob(f'{dataset}-w*_checks.csv'):
  for r in csv.DictReader(open(p)): checks[int(r['pos'])][r['move']]=r
 with tarfile.open(root/f'notes/candidate-diversity-20260930/archives/{dataset}.tar.gz') as t:
  for w in range(8):
   for r in csv.DictReader(io.TextIOWrapper(t.extractfile(f'w{w}.csv'))):
    pos=int(r['pos']);old[pos][r['arm']].add(r['move'])
    if r['chosen']=='1':chosen[pos][r['arm']]=r['move']
 refs=json.loads((out/f'{dataset}-references.json').read_text())
 metas={int(r['pos']):r for r in csv.DictReader(open(out/f'{dataset}-positions.meta.csv'))}
 pats={}
 for model in ['CSW24','CSW24_hsp']:
  pats[model]=collections.defaultdict(list)
  for p in out.glob(f'{model}.w*.csv'):
   for r in csv.DictReader(open(p)):
    p0=int(r['pos'])-offset
    if p0 in feats:pats[model][p0].append(r)
  assert len(pats[model])==len(feats)
 reconstruction=0
 for pos,fs in feats.items():
  fs.sort(key=lambda x:int(x['pool_index']));assert len(fs)==len(checks[pos])
  best=float(fs[0]['static_eq']);static=old[pos]['static25']
  b=set(r['move'] for r in sorted(fs,key=lambda r:-(float(r['static_eq'])+float(checks[pos][r['move']]['blocking_adjustment'])))[:25])
  s=set(r['move'] for r in sorted(fs,key=lambda r:-(float(r['static_eq'])+float(checks[pos][r['move']]['setup_adjustment'])))[:25])
  raw=set()
  for key in ['blocking_adjustment','setup_adjustment']:
   eligible=[r for r in fs if float(r['static_eq'])>=best-25]
   raw.update(r['move'] for r in sorted(eligible,key=lambda r:-float(checks[pos][r['move']][key]))[:5])
  q=set(r['move'] for r in sorted(fs,key=lambda r:-float(r['static_eq'])) if r['type']=='5' and float(r['static_eq'])>=best-35)
  q=set(r['move'] for r in sorted([r for r in fs if r['move'] in q],key=lambda r:-float(r['static_eq']))[:5])
  common=b|s|q;baseline=static|common
  if dataset=='overnight':
   assert static|b==old[pos]['threat_wide'],(dataset,pos,(static|b)^old[pos]['threat_wide'])
   assert static|s==old[pos]['setup_wide'],(dataset,pos,(static|s)^old[pos]['setup_wide'])
  else: assert static|b==old[pos]['threat_wide'],(dataset,pos,(static|b)^old[pos]['threat_wide'])
  reconstruction+=1
  wp={m:float(r['sim_wp'])*100 for m,r in refs[str(pos)].items()};assert baseline<=wp.keys()
  basebest=max(wp[m] for m in baseline)
  for model in pats:
   nominees=set(r['move'] for r in pats[model][pos]);replacement=common|nominees;added=replacement-baseline;removed=baseline-replacement
   covered=replacement&wp.keys();missing=replacement-wp.keys();newbest=max(wp[m] for m in covered)
   bestnew=max((wp[m] for m in added&wp.keys()),default=None)
   detail={'dataset':dataset,'model':model,'pos':pos,'phase':metas[pos]['phase'],'bag':int(metas[pos]['bag']),'baseline_count':len(baseline),'pat_count':len(replacement),'pat_nominees':len(nominees),'novel_count':len(added),'removed_count':len(removed),'unreferenced_count':len(missing),'covered_oracle_delta_pp':newbest-basebest,'baseline_oracle_wp':basebest,'replacement_covered_oracle_wp':newbest,'new_covered_best_wp':bestnew,'added':sorted(added),'removed':sorted(removed),'unreferenced':sorted(missing),'baseline_best_moves':[m for m in baseline if wp[m]==basebest],'replacement_covered_best_moves':[m for m in covered if wp[m]==newbest]}
   details.append(detail)
 assert reconstruction==len(feats)
 for model in pats:
  rs=[r for r in details if r['dataset']==dataset and r['model']==model]
  ds=[r['covered_oracle_delta_pp'] for r in rs]
  summary[f'{dataset}:{model}']={'N':len(rs),'reconstruction_validated_positions':reconstruction,'positions_changed':sum(r['novel_count']+r['removed_count']>0 for r in rs),'positions_novel':sum(r['novel_count']>0 for r in rs),'positions_unreferenced':sum(r['unreferenced_count']>0 for r in rs),'novel_mean':statistics.mean(r['novel_count'] for r in rs),'removed_mean':statistics.mean(r['removed_count'] for r in rs),'baseline_count_mean':statistics.mean(r['baseline_count'] for r in rs),'pat_count_mean':statistics.mean(r['pat_count'] for r in rs),'unreferenced_mean':statistics.mean(r['unreferenced_count'] for r in rs),'covered_oracle_gain_positions':sum(d>1e-7 for d in ds),'covered_oracle_loss_positions':sum(d<-1e-7 for d in ds),'covered_oracle_mean_delta_pp':statistics.mean(ds),'covered_oracle_max_gain_pp':max(ds),'covered_oracle_max_loss_pp':min(ds),'additive_covered_oracle_mean_delta_pp':statistics.mean(max(0,d) for d in ds)}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');(out/'positions.json').write_text(json.dumps(details,indent=2)+'\n')
print(json.dumps(summary,indent=2))
print('Largest gains and losses:')
for dataset in ['overnight','blocking']:
 rs=[r for r in details if r['dataset']==dataset and r['model']=='CSW24'];rs.sort(key=lambda r:r['covered_oracle_delta_pp'])
 for r in rs[:3]+rs[-3:]:print(json.dumps(r))
