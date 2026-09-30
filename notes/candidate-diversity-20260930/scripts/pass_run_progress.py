import csv,json,time
from pathlib import Path
D=Path('/Users/olaugh/sources/magpie-pat-postx/postx/threatgames/pass_relative_20260929')
def count(files,arms):
 data={}
 for file in files:
  for row in csv.DictReader(file.open()):
   if row.get('move') and row.get('arm') and row.get('chosen') and row.get('candidates'):
    data.setdefault(row['pos'],{}).setdefault(row['arm'],{})[row['move']]=row
 return sum(all(arm in rr and len(rr[arm])==int(next(iter(rr[arm].values()))['candidates']) and sum(int(row['chosen']) for row in rr[arm].values())==1 for arm in arms) for rr in data.values())
status={'primary':count(list(D.glob('w[0-7].csv'))+([D/'r0.csv'] if (D/'r0.csv').exists() else []),['static25','threat_wide','setup_wide','signals','exchange','combined','reference']), 'width':count(D.glob('v[0-7].csv'),['static25_fulltime','adaptive_static','static60','adaptive_signals']), 'depth6':count(D.glob('d[0-7].csv'),['static25_fulltime','adaptive_static','static60','adaptive_signals','reference']), 'matched':count(D.glob('m[0-7].csv'),['static_setup_count','static_combined_count']), 'checks128':count(D.glob('c[0-7].csv'),['setup_conditioned','signals_conditioned','combined_conditioned']), 'quota':count((D/'tile_followup').glob('w[0-3].csv'),['static25_fulltime','tile_quota','tile_exchange','reference']), 'elapsed_minutes':round((time.time()-json.loads((D/'manifest.json').read_text())['started_epoch'])/60,1), 'complete':(D/'complete.json').exists()}
if (D/'finalizer_stage.json').exists():status['finalizer']=json.loads((D/'finalizer_stage.json').read_text())
status['failures']=[file.name for file in D.glob('*_status.json') if any(json.loads(file.read_text()).get('exit_codes',[]))]
print(json.dumps(status))
