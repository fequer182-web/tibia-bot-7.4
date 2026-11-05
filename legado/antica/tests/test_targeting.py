from pathlib import Path
import json,struct,sys,tempfile
from unittest.mock import patch
root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'src'))
from cromin.core.targeting import parse_names,matches
from cromin.readers.world import WorldReader,RatDataError
from cromin.actions.target_attack import attack_target
from cromin.actions.common import SkipCycle
from cromin.core.controller import Controller
from cromin.core.state import Bridge,ATTACK,LOOT
assert parse_names('Rat, bat; RAT\nSnake')==['Rat','bat','Snake']
assert matches('BAT',['bat']) and not matches('Dragon Lord',['Dragon'])
assert matches('Dragon Lord',[],True)
for value in ([1],'x'*129,42):
 try:parse_names(value)
 except ValueError:pass
 else:raise AssertionError(value)
class Memory:
 def __init__(self):self.data={}
 def alloc(self,a,n):self.data.update({a+i:0 for i in range(n)})
 def put(self,a,fmt,*values):self.data.update({a+i:b for i,b in enumerate(struct.pack(fmt,*values))})
 def exact(self,a,n):
  try:return bytes(self.data[a+i] for i in range(n))
  except KeyError:raise OSError('Unreadable fixture')
BASE=0x140000000;m=Memory();h=0x100000;player=0x110000
m.alloc(h,0x38);m.put(h+0x19,'B',1);m.alloc(player,0x80)
m.put(BASE+0xE52A48,'Q',player);m.put(player,'Q',BASE+0xAE99E0);m.put(player+0x30,'<IIH',100,100,7)
entries=[('Rat',101,100,7,100,0xAF61D8),('Bat',102,100,7,100,0xAF61D8),('Very Long Monster Name',103,100,7,100,0xAF61D8),('Dragon',101,100,8,100,0xAF61D8),('Snake',101,100,7,0,0xAF61D8),('Player',101,100,7,100,0xAE99E0),('NPC',101,100,7,100,0xAF0000)]
m.put(BASE+0xE52F30,'<QQ',h,len(entries));m.put(h+8,'Q',0x200000)
for i,(name,x,y,z,hp,vt) in enumerate(entries):
 node=0x200000+i*0x1000;obj=0x300000+i*0x1000;rid=42+i
 m.alloc(node,0x38);m.alloc(obj,0x80)
 m.put(node,'<QQQ',h,h if i==0 else node-0x1000,h if i==len(entries)-1 else node+0x1000)
 m.put(node+0x20,'I',rid);m.put(node+0x28,'<QQ',obj,obj-16)
 m.put(obj,'Q',BASE+vt);m.put(obj+0x20,'<IIH',x,y,z);m.put(obj+0x40,'I',rid);m.put(obj+0x69,'B',hp)
 data=name.encode()+b'\0'
 if len(name)<=15:m.put(obj+0x48,f'{len(data)}s',data);capacity=15
 else:
  address=0x400000+i*0x1000;m.alloc(address,len(data));m.put(address,f'{len(data)}s',data);m.put(obj+0x48,'Q',address);capacity=31
 m.put(obj+0x58,'<QQ',len(name),capacity)
w=WorldReader(m,BASE)
assert [v['name'] for v in w.monsters_on_floor('bat, RAT')]==['Rat','Bat']
assert [v['name'] for v in w.monsters_on_floor([],True)]==['Rat','Bat','Very Long Monster Name']
assert w.monsters_on_floor('Dragon')==[]
assert w.monster(44)['name']=='Very Long Monster Name'
assert [v['name'] for v in w.rats_on_floor()]==['Rat']
m.put(0x302000+0x60,'Q',14)
assert [v['name'] for v in w.monsters_on_floor([],True)]==['Rat','Bat']
print('PASS: nomes, maiusculas, qualquer Monster, nomes longos; exclui jogador, NPC, morto, outro andar e nome invalido')
selected={'id':43,'object':'0x301000','control':'0x300ff0','name':'Bat','hp':100,'position':{'x':102,'y':100,'z':7}}
def result():return {'exit_code':0,'report':dict.fromkeys(('started','returned','context_restored','stack_slot_restored','detached','attack_requested','attack_returned','owned_reference_handled'),True)|{'rat_id':43,'rat_object':selected['object'],'diagnostics':[]}}
class Watch:
 def __init__(self,scenario):self.reads=0;self.scenario=scenario
 def player(self):return {'x':100,'y':100,'z':7}
 def monster(self,rid):
  assert rid==43;self.reads+=1
  if self.reads==1:return dict(selected,object='0x99999') if self.scenario=='reuse' else dict(selected)
  if self.scenario=='lost':raise RatDataError(dict(selected,actual_id=43,reason='invalid_position_or_health',hp=100,position={'x':65535,'y':65535,'z':255}))
  return None
for scenario in ('normal','reuse','lost','native_failure'):
 calls=[];report={}
 def runner(command,prefix):
  calls.append(command);assert '--monster-name-hex' in command and command[command.index('--monster-name-hex')+1]=='426174'
  assert command[command.index('--object')+1]==selected['object']
  return {'exit_code':2,'report':{}} if scenario=='native_failure' else result()
 try:attack_target(None,Watch(scenario),1,2,selected,report,lambda:False,runner=runner,sleep=lambda _:None)
 except SkipCycle:assert scenario=='reuse' and not calls
 except RuntimeError:assert scenario=='native_failure' and len(calls)==1
 else:
  assert len(calls)==1 and report['status']==('target_lost' if scenario=='lost' else 'target_left_world')
  assert not report['loot_supported']
print('PASS: ataque unico por nome/objeto, sem loot incerto, rejeita reutilizacao e falha nativa sem repetir')
with tempfile.TemporaryDirectory(dir=root/'data') as folder:
 bridge=Bridge()
 with bridge.lock() as s:s.targetName='Bat';s.targetAny=True;s.flags=ATTACK|LOOT
 c=Controller(None,BASE,1,2,bridge,folder=folder)
 class Targets:
  def u64(self,address):return 0
  def monsters_on_floor(self,names,any_monster):assert names==['Bat'] and any_monster;return [selected]
 c.world=Targets();c.pulse=lambda:False
 def fake_attack(*args,**kwargs):
  assert kwargs['selected']==selected and kwargs['loot_policy'].corpses==(3994,)
  args[6].update(status='target_left_world',confirmed=True)
 with patch('cromin.core.controller.cycle',side_effect=fake_attack) as call:c.step();assert call.call_count==1
 c.save_settings(bridge.snapshot());data=json.loads(c.settings.read_text())
 assert data['target_names']==['Bat'] and data['target_any']
 reopened=Bridge();Controller(None,BASE,1,2,reopened,folder=folder)
 assert reopened.snapshot().targetAny and reopened.snapshot().targetName=='Bat' and reopened.snapshot().flags==0
print('PASS: controlador usa filtro, persiste e carrega sem ligar automacoes; monstro nao exige bag de Rat')
