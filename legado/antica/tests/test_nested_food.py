from pathlib import Path
import copy,sys,tempfile,runpy
from unittest.mock import patch
root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'src'))
from cromin.actions.loot_transfer import stable
from cromin.actions.common import SkipCycle
from cromin.actions.food import AutoFood
from cromin.actions.item_loot import open_configured
from cromin.core.loot_policy import LootPolicy
from cromin.core.controller import Controller
from cromin.core.state import Bridge,LOOT,FOOD,CAVE,Position

class Memory:
 def alive(self):pass
class Sequence:
 def __init__(self,values):self.values=iter(values)
 def scan(self):return next(self.values)
with patch('time.sleep',lambda _:None):
 assert stable(Sequence([[1],[2],[3],[3]]),Memory())==[3]
 try:stable(Sequence([[1],[2]]*3),Memory())
 except SkipCycle as error:assert 'Containers mudaram entre leituras' in str(error)
 else:raise AssertionError('Inconsistent containers accepted')
with tempfile.TemporaryDirectory(dir=root/'data') as folder:
 b=Bridge()
 with b.lock() as s:s.flags=LOOT
 c=Controller(Memory(),0,1,2,b,folder=folder);reads=[0]
 def step():
  reads[0]+=1
  if reads[0]==1:raise SkipCycle('Containers mudaram entre leituras')
  assert b.snapshot().flags&LOOT;c.quit=True
 c.step=step
 with patch('time.sleep',lambda _:None):c.run()
 assert c.fault is None and b.snapshot().flags&LOOT
print('PASS: leitura instavel tenta novamente, descarta leitura e mantem modulos sem fault')

fx=runpy.run_path(str(root/'tests/test_item_loot.py'));item,container,result=[fx[k] for k in ('item','container','result')];World=fx['World'];CORPSE=fx['CORPSE']
world=World([item(2853,0,0,0x710000)]);child=container(2,2853,[item(3031,0,4,0x720000)]);calls=[];report={}
def arg(command,key):return command[command.index(key)+1]
def runner(command,prefix):
 calls.append(command);name=Path(command[0]).name
 if name=='OTC_Corpse_Open_Once.exe':
  world.containers.append(world.source);return result(raw_rax_at_return='0x1',corpse_item_id=4242,corpse_object=CORPSE['object'])
 if name=='OTC_Item_Use_Once.exe':
  assert arg(command,'--action')=='open' and arg(command,'--item-id')==2853 and arg(command,'--source')==1
  world.containers.append(child)
  return result(raw_rax_at_return='0x2',action='open_bag',item_id=2853,borrowed_item='0x710000',source_container=1,source_slot=0)
 assert name=='OTC_Loot_Once.exe' and arg(command,'--source')==2
 source_item=child['items'].pop();child['item_count']=0;world.bag['items']=[dict(source_item,address='0x730000')];world.bag['item_count']=1
 return result(item_id=3031,borrowed_item=source_item['address'],source_container=2,source_slot=0,requested_count=4,destination={'x':65535,'y':64,'z':0})
with patch('time.sleep',lambda _:None):open_configured(Memory(),world,1,2,0,report,lambda:False,LootPolicy.create([4242],[3031],[]),None,None,False,runner,lambda:None)
assert report['confirmed'] and report['collected']==4 and len(report['opened_bags'])==1
assert len(calls)==3 and world.source['items'][0]['id']==2853
print('PASS: abre corpo, abre bag 2853 interna e recolhe moeda da janela filha sem mover bag nem trocar destino')

class FoodWorld:
 def __init__(self,from_monster):
  self.items=[item(3577,0,2,0x750000)];self.c=container(1 if from_monster else 0,2853,self.items)
 def scan(self):return copy.deepcopy([self.c])
for monster in (False,True):
 for consume in (False,True):
  world=FoodWorld(monster);now=[0.];sent=[];events=[]
  def clock():return now[0]
  def sleep(_):now[0]+=0.5
  def runner(command,prefix):
   sent.append(command);assert arg(command,'--action')=='use' and arg(command,'--item-id')==3577
   if consume:world.items[0]['raw_count_or_subtype']-=1
   return result(action='use_food',item_id=3577,borrowed_item='0x750000',source_container=1 if monster else 0,source_slot=0)
  food=AutoFood(Memory(),world,1,2,runner,lambda *a:events.append(a),clock,sleep)
  with patch('time.sleep',lambda _:None):
   assert not food.tick(False,[3577]) and not sent
   assert food.tick(True,[3577]);assert len(sent)==1
   assert events[-1][1]['confirmed']==consume
   now[0]+=10;assert not food.tick(True,[3577]) and len(sent)==1
   if not consume:
    now[0]+=20;assert not food.tick(True,[3577]) and len(sent)==1
print('PASS: Auto Food 3577 da bag do jogador e monstro, confirma uso por leitura; cooldown/backoff e estado desligado')

from cromin.core.profiles import validate_profile
with tempfile.TemporaryDirectory(dir=root/'data') as folder:
 b=Bridge()
 with b.lock() as s:s.foodIds='3577, 3582'
 c=Controller(Memory(),0,1,2,b,folder=folder);c.save_settings(b.snapshot())
 fresh=Bridge();Controller(Memory(),0,1,2,fresh,folder=folder)
 assert fresh.snapshot().foodIds=='3577, 3582' and fresh.snapshot().flags==0
profile=validate_profile({'hp':80,'cooldown':2000,'destination':0,'spell':'','food_ids':[3577,3582],'loop':False})
assert profile['food_ids']==[3577,3582] and profile['loop']
b=Bridge()
with b.lock() as s:s.flags=FOOD|CAVE
b.disable();assert b.snapshot().flags==0
print('PASS: food IDs persistidos, inicio desligado, Parar inclui Food e perfil antigo nao desliga repeticao')
