from pathlib import Path
import copy,sys
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'src'))
from cromin.core.profiles import validate_profile
base={'hp':80,'cooldown':2000,'destination':0,'spell':'exura','light':31,'target_follow':True,'loop':True,'target_names':['Rat'],'target_any':False,'waypoints':[{'x':100,'y':101,'z':7}]}
parsed=validate_profile(base);assert parsed==dict(base,corpse_ids=[3994],loot_ids=[3031],drop_ids=[],drop_scope='corpse',food_ids=[3577])
parsed['waypoints'][0]['x']=123;assert base['waypoints'][0]['x']==100
for change in ({'hp':0},{'target_follow':'sim'},{'cooldown':1},{'spell':'x'*16},{'waypoints':[{'x':0,'y':1,'z':7}]},{'waypoints':[{'x':True,'y':1,'z':7}]}):
    try:validate_profile(copy.deepcopy(base)|change)
    except ValueError:pass
    else:raise AssertionError(change)
for other in ({},{'timestamp':'0x6AD91836'},[],None):
    try:validate_profile(other)
    except ValueError:pass
    else:raise AssertionError(other)
assert validate_profile(base|{'light':7})['light']==31
old={k:v for k,v in base.items() if k not in ('target_names','target_any')}
assert validate_profile(old)['target_names']==['Rat'] and not validate_profile(old)['target_any']
print('PASS: configuracao Cromin valida e copiada; rejeita limites, rota e formatos de perfil diferentes')
