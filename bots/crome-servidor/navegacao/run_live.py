from pathlib import Path
import json,os,subprocess,sys,time,hashlib
root=Path(__file__).resolve().parent;workspace=root.parent.parent.parent
os.chdir(workspace)
meta=json.loads((root.parent/'batch_control.json').read_text());r=Path(meta['runtime'])
cases=json.loads((root/'live_cases.json').read_text())
domain=hashlib.sha256((r/'data/world/world.otbm').read_bytes()+(r/'data/items/items.otb').read_bytes()+(r/'data/items/items.xml').read_bytes()+b'nav-policy-v1').hexdigest()
assert all(c['domain']==domain for c in cases),'Mapa/catalogo mudaram; gere um dominio novo antes de reutilizar a politica'
subprocess.run([sys.executable,str(root.parent/'check_lua_syntax.py'),str(root.parent/'batch_driver.lua'),str(r/'data/crome_learning/navigation.lua'),str(r/'data/crome_learning/policy.lua'),str(r/'data/crome_learning/controller.lua')],check=True)
print('Duas sessoes de 55s; cinco perfis de teste. Memoria e politica serao continuadas. HP/condicoes sao preparados antes de cada sessao.',flush=True)
env=os.environ.copy();env['CROME_CASES']='';env['CROME_EXTERNAL_CASES']=str(root/'live_cases.json');env['CROME_RUN_TAG']='navegacao_'+str(int(time.time()))
subprocess.run([sys.executable,str(root.parent/'run_batch.py')],env=env,check=True)
