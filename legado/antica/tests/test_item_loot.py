from pathlib import Path
import copy,sys,tempfile,functools
from unittest.mock import patch
root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'src'))
from cromin.core.loot_policy import LootPolicy,parse_ids
from cromin.core.profiles import validate_profile
from cromin.core.state import Bridge,LOOT,ATTACK
from cromin.core.controller import Controller
from cromin.actions.item_loot import open_configured,source_change,destination_change,move_item
from cromin.actions.common import SkipCycle

assert parse_ids('3994, 4242;3994\n5000')==[3994,4242,5000]
for bad in ('0','-1','65536','3.5','True','１２３',[True],None):
 try:parse_ids(bad)
 except ValueError:pass
 else:raise AssertionError(bad)
try:LootPolicy.create(take=[3031],drop=[3031])
except ValueError:pass
else:raise AssertionError('Ambiguous loot/drop accepted')
base={'hp':80,'cooldown':2000,'destination':0,'spell':''}
assert validate_profile(base)['corpse_ids']==[3994]
policy=LootPolicy.create('4242','3031,3552','3492')
profile=validate_profile(dict(base,**policy.settings()));assert profile['drop_ids']==[3492]
with tempfile.TemporaryDirectory(dir=root/'data') as folder:
 b=Bridge()
 with b.lock() as s:s.corpseIds='4242';s.lootIds='3031, 3552';s.dropIds='3492'
 c=Controller(None,0,1,2,b,folder=folder);c.save_settings(b.snapshot())
 reopened=Bridge();Controller(None,0,1,2,reopened,folder=folder)
 assert LootPolicy.from_state(reopened.snapshot())==policy and reopened.snapshot().flags==0
print('PASS: IDs validados, conflito rejeitado, perfis antigos migrados e novas listas persistidas sem ligar modulos')

P={'x':100,'y':100,'z':7};CORPSE={'object':'0x400000','control':'0x3ffff0','id':4242,'position':{'x':100,'y':101,'z':7}}
def item(i,slot,raw,address):return {'id':i,'slot':slot,'raw_count_or_subtype':raw,'address':hex(address)}
def container(cid,itemid,items):return {'container_id':cid,'address':hex(0x500000+cid*4096),'bag_item':{'id':itemid,'address':hex(0x600000+cid*4096)},'capacity':8,'item_count':len(items),'items':items}
flags=dict.fromkeys(('started','returned','context_restored','stack_slot_restored','detached'),True)
def result(**data):return {'exit_code':0,'report':dict(flags,diagnostics=[],**data)}
class Memory:
 def alive(self):pass
class World:
 def __init__(self,items):
  self.source=container(1,4242,copy.deepcopy(items));self.bag=container(0,1988,[]);self.containers=[self.bag];self.ground=[];self.tile_available=True;self.corpse_ids=(4242,)
 def scan(self):return copy.deepcopy(self.containers)
 def player(self):return dict(P)
 def nearby_corpses(self,*a):return [copy.deepcopy(CORPSE)]
 def corpses_at(self,p):assert p==CORPSE['position'];return self.nearby_corpses()
 def tile_pair(self,p):assert p==P;return (0x800000,(0x900000,0x8ffff0)) if self.tile_available else None
 def items_at(self,p):assert p==P;return copy.deepcopy(self.ground)

def run_scenario(items,policy,no_change=False,missing_tile=False):
 world=World(items);world.tile_available=not missing_tile;calls=[];report={};nextobj=[0xa00000]
 def runner(command,prefix):
  calls.append(command);name=Path(command[0]).name
  def arg(key):return command[command.index(key)+1]
  if name=='OTC_Corpse_Open_Once.exe':
   assert arg('--corpse-id')==4242
   world.containers.append(world.source)
   return result(raw_rax_at_return='0x1',corpse_item_id=4242,corpse_object=CORPSE['object'])
  assert name=='OTC_Loot_Once.exe' and arg('--source-item-id')==4242
  iid=arg('--item-id');obj=arg('--item-object');count=arg('--count');isdrop='--ground' in command
  src=next(i for i in world.source['items'] if i['address']==obj)
  assert iid in (policy.drop if isdrop else policy.take)
  assert count==(src['raw_count_or_subtype'] if iid==3031 else 1)
  if isdrop:assert [arg(k) for k in ('--x','--y','--z')]==[100,100,7]
  else:assert arg('--destination')==0 and arg('--destination-object')==world.bag['address']
  if not no_change:
   raw=src['raw_count_or_subtype'];moved_raw=count
   if iid==3552 and raw>1:src['raw_count_or_subtype']-=1
   else:
    world.source['items'].remove(src)
    if iid!=3031:moved_raw=raw
   for n,i in enumerate(world.source['items']):i['slot']=n
   world.source['item_count']=len(world.source['items'])
   target=world.ground if isdrop else world.bag['items'];nextobj[0]+=16
   target.append(item(iid,len(target),moved_raw,nextobj[0]));world.bag['item_count']=len(world.bag['items'])
  return result(item_id=iid,borrowed_item=obj,source_container=1,source_slot=arg('--slot'),requested_count=count,destination=dict(P) if isdrop else {'x':65535,'y':64,'z':0},drop_to_player_tile=isdrop)
 now=[0]
 def clock():now[0]+=0.5;return now[0]
 with patch('time.sleep',lambda _:None),patch('cromin.actions.item_loot.time.monotonic',clock),patch('cromin.actions.item_loot.move_item',functools.partial(move_item,clock=clock,sleep=lambda _:None)):
  try:open_configured(Memory(),world,1,2,0,report,lambda:False,policy,None,None,False,runner,lambda:None)
  except SkipCycle:
   assert no_change or missing_tile
  else:assert not no_change and not missing_tile and report['confirmed']
 return world,calls,report
items=[item(3031,0,15,0x700000),item(3552,1,2,0x700010),item(3492,2,7,0x700020),item(9999,3,0,0x700030)]
w,calls,report=run_scenario(items,LootPolicy.create([4242],[3031,3552],[3492]))
assert [i['id'] for i in w.source['items']]==[9999]
assert [i['id'] for i in w.bag['items']]==[3031,3552,3552]
assert len(w.ground)==1 and w.ground[0]['id']==3492 and w.ground[0]['raw_count_or_subtype']==7
assert report['collected']==17 and report['dropped']==1 and len(calls)==5
# Drop-only works without a destination bag being selected/open.
w,calls,r=run_scenario([item(3492,0,0,0x700000)],LootPolicy.create([4242],[],[3492]))
assert r['dropped']==1 and not w.bag['items']
w,calls,r=run_scenario([item(3492,0,0,0x700000)],LootPolicy.create([4242],[],[3492]),missing_tile=True)
assert len(calls)==1 and not w.ground
w,calls,r=run_scenario([item(3492,0,0,0x700000)],LootPolicy.create([4242],[],[3492]),no_change=True)
assert len(calls)==2 and not r['confirmed']
# Subtypes/charges must never be counted as seven units or merged as a stack.
assert destination_change({}, {'new':7},('whole',7))
assert not destination_change({'old':10},{'old':17},('whole',7))
assert destination_change({'old':10},{'old':11},('units',1))
print('PASS: corpo configurado, loot multiplo, item unitario/subtipo, Drop somente do corpo no SQM atual, item ignorado e chamada nao confirmada sem repetir')

# A non-Rat target uses the configured corpse association and the same loot path.
from cromin.actions.combat_cycle import cycle
chosen={'id':42,'object':'0xb00000','control':'0xaffff0','name':'Bat','hp':100,'position':{'x':100,'y':101,'z':7}}
class CombatWorld(World):
 def __init__(self):super().__init__([]);self.reads=0;self.dead=False
 def monster(self,rid):
  assert rid==42;self.reads+=1
  if self.reads==1:return copy.deepcopy(chosen)
  self.dead=True;return None
 def nearby_corpses(self,*a):return []
 def corpses_at(self,p):return [copy.deepcopy(CORPSE)] if self.dead and p==CORPSE['position'] else []
combat_calls=[];collected=[];r={};world=CombatWorld()
def attack_runner(command,prefix):
 assert '--monster-name-hex' in command and command[command.index('--monster-name-hex')+1]=='426174'
 combat_calls.append(command)
 return result(attack_requested=True,attack_returned=True,owned_reference_handled=True,rat_id=42,rat_object=chosen['object'],rat_position_at_request=chosen['position'])
def configured_collect(*args,**kwargs):
 assert kwargs['loot_policy']==policy and args[7]==CORPSE['position']
 collected.append(True);args[5].update(confirmed=True,collected=1)
with patch('time.sleep',lambda _:None),patch('cromin.actions.combat_cycle.open_and_collect',side_effect=configured_collect):
 cycle(Memory(),world,1,2,0,10,r,lambda:False,runner=attack_runner,selected=chosen,loot_policy=policy)
assert len(combat_calls)==1 and collected==[True] and r['confirmed'] and r['new_corpse']['id']==4242
print('PASS: Monster por nome associa corpo configurado e encaminha listas de loot/drop sem repetir ataque')
