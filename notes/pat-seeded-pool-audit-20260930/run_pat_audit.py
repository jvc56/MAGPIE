import pathlib, subprocess, os, json, hashlib, time
root=pathlib.Path('/Users/olaugh/sources/magpie-pat-simcmp')
out=root/'notes/pat-seeded-pool-audit-20260930'
lines=[]; mapping={}
for dataset,offset in [('overnight',0),('blocking',2000)]:
 for line in (out/(dataset+'-positions.cgp')).read_text().splitlines():
  pos,cgp=line.split(',',1); key=int(pos)+offset
  assert key not in mapping
  mapping[key]={'dataset':dataset,'pos':int(pos)};lines.append(f'{key},{cgp}\n')
assert len(lines)==1600
(out/'combined.cgp').write_text(''.join(lines))
(out/'position_map.json').write_text(json.dumps(mapping,indent=2)+'\n')
protocol={'kind':'Exploratory untimed nomination audit; no new simulations','models':['CSW24','CSW24_hsp'],'N':1600,'root_pat_nominees':25,'rollout':'No rollouts run; reused references had no-PAT static rollouts','reference_limitation':'Old reference universe only; no evaluation of previously uncovered moves','sha256':{}}
for p in [root/'bin/magpie_test',out/'combined.cgp']+[root/f'data/strategy/{m}.pat' for m in protocol['models']]:
 protocol['sha256'][str(p)]=hashlib.sha256(p.read_bytes()).hexdigest()
(out/'PROTOCOL.json').write_text(json.dumps(protocol,indent=2)+'\n')
for model in protocol['models']:
 procs=[]
 for worker in range(8):
  env=os.environ.copy();env.update(PCD_IN=str(out/'combined.cgp'),PCD_PAT_AUDIT_OUT=str(out/f'{model}.w{worker}.csv'),PCD_PAT=model,PCD_WORKER=str(worker),PCD_NUM_WORKERS='8')
  log=open(out/f'{model}.w{worker}.log','w')
  p=subprocess.Popen(['./bin/magpie_test','patcanddiversity'],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT);procs.append((p,log))
 print('Started',model,[p.pid for p,log in procs],flush=True)
 failures=[]
 for p,log in procs:
  rc=p.wait();log.close()
  if rc: failures.append((p.pid,rc))
 if failures: raise RuntimeError(failures)
 print('Completed',model,flush=True)
(out/'audit_complete.json').write_text(json.dumps({'time':time.time(),'N':1600})+'\n')
