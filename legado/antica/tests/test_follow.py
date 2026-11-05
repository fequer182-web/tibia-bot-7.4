from pathlib import Path
import json,sys,tempfile
from unittest.mock import patch
root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'src'))
from cromin.actions.combat_cycle import cycle
from cromin.actions.target_attack import attack_target
from cromin.readers.world import RatDataError
from cromin.core.controller import Controller
from cromin.core.state import Bridge,ATTACK
selected={'id':42,'object':'0x210000','control':'0x20fff0','name':'Rat','hp':100,'position':{'x':101,'y':100,'z':7}}
class World:
 def player(self):return {'x':100,'y':100,'z':7}
 def nearby_corpses(self,*args):return []
 def rats_on_floor(self):return [selected]
 def rat(self,rid):raise RatDataError(dict(selected,actual_id=42,reason='invalid_position_or_health',position={'x':65535,'y':65535,'z':255}))
 def monster(self,rid):
  self.reads=getattr(self,'reads',0)+1
  return dict(selected,name='Bat') if self.reads==1 else None
 def u64(self,address):return 0
 def monsters_on_floor(self,*args):return [dict(selected,name='Bat')]
for generic in (False,True):
 for follow in (False,True):
  for verified in (False,True):
   calls=[]
   def runner(command,prefix):
    calls.append(command);assert command[command.index('--chase-mode')+1]==int(follow)
    return {'exit_code':0,'report':dict.fromkeys(('started','returned','context_restored','stack_slot_restored','detached','attack_requested','attack_returned','owned_reference_handled'),True)|{'rat_id':42,'rat_object':selected['object'],'rat_position_at_request':selected['position'],'chase_mode_requested':int(follow),'chase_verified':verified,'diagnostics':[]}}
   try:
    if generic:attack_target(None,World(),1,2,dict(selected,name='Bat'),{},lambda:False,runner=runner,sleep=lambda _:None,chase=follow)
    else:cycle(None,World(),1,2,0,1,{},lambda:False,runner=runner,collect=lambda:False,chase=follow)
   except RuntimeError:assert not verified
   else:assert verified
   assert len(calls)==1
with tempfile.TemporaryDirectory(dir=root/'data') as folder:
 for generic in (False,True):
  for follow in (False,True):
   bridge=Bridge()
   with bridge.lock() as s:s.flags=ATTACK;s.targetFollow=follow;s.targetName='Bat' if generic else 'Rat'
   c=Controller(None,0,1,2,bridge,folder=folder);c.world=World();c.pulse=lambda:False
   # No settings file exists until the persistence check below.
   with patch('cromin.core.controller.attack_target' if generic else 'cromin.core.controller.cycle') as call:
    c.step();assert call.call_count==1 and call.call_args.kwargs['chase']==follow
 c.settings.write_text(json.dumps({'hp':80,'cooldown':2000,'destination':0,'spell':'','light':7,'target_follow':False}),encoding='utf-8')
 bridge=Bridge();Controller(None,0,1,2,bridge,folder=folder)
 assert not bridge.snapshot().targetFollow and bridge.snapshot().lightLevel==31 and bridge.snapshot().flags==0
print('PASS: follow ligado/desligado encaminhado para Rat e Monster, confirma leitura sem repetir; luz migra para 31')
