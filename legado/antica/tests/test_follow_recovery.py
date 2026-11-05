from pathlib import Path
import struct,sys,tempfile
from types import SimpleNamespace
from unittest.mock import patch
root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'src'))
from cromin.actions.follow_watch import FollowWatch
from cromin.readers.world import WorldReader,RatDataError
from cromin.actions.combat_cycle import tracked_observation
from cromin.core.controller import Controller
from cromin.core.state import Bridge,ATTACK,CAVE,LOOP,Position
from cromin.ui.panel import App

selected={'id':42,'object':'0x300000','control':'0x2ffff0','name':'Rat','hp':100,'position':{'x':104,'y':100,'z':7}}
class World:
 def __init__(self):self.active=int(selected['object'],16);self.followed=0;self.current=dict(selected);self.position={'x':100,'y':100,'z':7}
 def u64(self,address):return self.active if address==0xE52A50 else self.followed
 def monster(self,rid):return self.current
 def player(self):return dict(self.position)

def success():return {'exit_code':0,'report':dict.fromkeys(('started','returned','context_restored','stack_slot_restored','detached','follow_refresh_only','follow_refresh_reset_verified','follow_refresh_returned','chase_verified'),True)|{'rat_id':42,'rat_object':selected['object'],'attack_requested':False,'diagnostics':[]}}
now=[0];enabled=[True];calls=[];events=[];world=World();outcome=[success()]
def runner(command,prefix):
 assert command[-2:]==['--refresh-chase',1] and '--chase-mode' not in command
 calls.append(command);return outcome[0]
w=FollowWatch(world,0,1,2,runner,lambda:enabled[0],lambda *a:events.append(a),lambda:now[0])
def tick(t):now[0]=t;w.observe(selected,world.current,world.player())
tick(0);tick(1.9);assert not calls
tick(2);assert len(calls)==1
tick(2.5);tick(4.9);assert len(calls)==1
tick(5);tick(8);tick(11);assert len(calls)==3 and events[-1][1]['status']=='stationary_retry_limit'
# Movement re-arms the watchdog; it must also detect a later stop after walking.
world.position['x']=101;tick(12);tick(13.9);assert len(calls)==3
tick(14);assert len(calls)==4
world.position['x']=103;tick(18);tick(30);assert len(calls)==4 # adjacent: no spam while fighting
world.position['x']=100;tick(31);world.active=0x444444;tick(34);assert len(calls)==4
world.active=int(selected['object'],16);world.followed=0x444444;tick(37);assert len(calls)==4
world.followed=0;enabled[0]=False;tick(40);assert len(calls)==4
enabled[0]=True;tick(41);tick(43);assert len(calls)==5
# Calls that returned safely but refused the requested state never cause retries.
outcome[0]=success();outcome[0]['exit_code']=2;outcome[0]['report']['follow_refresh_returned']=False;outcome[0]['report']['diagnostics']=['refused']
tick(46);assert w.disabled and len(calls)==6
tick(60);world.position['x']=99;tick(61);tick(65);assert len(calls)==6
# An unconfirmed restore remains a real fault, rather than silently issuing another call.
w.clear();world.position['x']=100;outcome[0]=success();outcome[0]['report']['context_restored']=False
tick(70)
try:tick(72)
except RuntimeError:pass
else:raise AssertionError('Uncertain restore ignored')
# A native preflight skip means no call happened and keeps the session usable.
w.clear();outcome[0]={'exit_code':1,'report':{'refresh_preflight_skipped':True,'follow_refresh_only':True,'started':False,'detached':True,'diagnostics':[]}}
tick(80);tick(82);assert not w.disabled
print('PASS: Follow parado apos andar, intervalo, limite, mesmo alvo/andar, proximidade, desligado e falhas sem repetir ataque')

# Real WorldReader fixture reproduces cleared name + off-map coordinates in screenshot.
class Memory:
 def __init__(self):self.data={}
 def put(self,a,fmt,*v):self.data.update({a+i:b for i,b in enumerate(struct.pack(fmt,*v))})
 def exact(self,a,n):return bytes(self.data[a+i] for i in range(n))
m=Memory();node=0x200000;obj=int(selected['object'],16);control=int(selected['control'],16);raw=bytearray(0x70)
struct.pack_into('<Q',raw,0,0xAF61D8);struct.pack_into('<I',raw,0x40,42)
struct.pack_into('<IIH',raw,0x20,65535,65535,255);struct.pack_into('<QQ',raw,0x58,0,15);raw[0x69]=20
m.put(obj,'112s',raw);m.put(node+0x28,'QQ',obj,control)
reader=WorldReader(m,0);reader.tree_find=lambda *a:node
for hp in (20,0):
 m.put(obj+0x69,'B',hp)
 try:reader.rat(42)
 except RatDataError as e:assert e.details['reason']=='invalid_position_or_health' and e.details['name_length']==0
 else:raise AssertionError('Off-map object accepted')
# ID/vtable/control still take priority over off-map classification.
m.put(obj+0x40,'I',43)
try:reader.rat(42)
except RatDataError as e:assert e.details['reason']=='identity_changed'
else:raise AssertionError('Reused ID accepted')
print('PASS: nome apagado fora do mapa classificado como alvo indisponivel; identidade continua protegida')

# Re-enable Cavebot always enables repeat, including legacy settings with loop false.
class Variable:
 def __init__(self,value):self.value=value
 def get(self):return self.value
 def set(self,value):self.value=value
b=Bridge()
with b.lock() as s:s.waypointCount=2;s.waypoints[0]=Position(100,100,7);s.waypoints[1]=Position(101,100,7)
a=SimpleNamespace(session=SimpleNamespace(connected=True,bridge=b),preview=False,variables={CAVE:Variable(True)},loop=Variable(False),apply=lambda:True)
App.toggle(a,CAVE);assert b.snapshot().flags&CAVE
with tempfile.TemporaryDirectory(dir=root/'data') as folder:
 c=Controller(None,0,1,2,b,folder=folder);c.world=World();c.world.position={'x':101,'y':100,'z':7}
 with b.lock() as s:s.waypointIndex=1
 c.waypoint(b.snapshot());assert b.snapshot().flags&CAVE and b.snapshot().waypointIndex==0
# Watchdog remains in use after the observation cycle expires.
with tempfile.TemporaryDirectory(dir=root/'data') as folder:
 b=Bridge()
 with b.lock() as s:s.flags=ATTACK
 c=Controller(None,0,1,2,b,folder=folder);c.pulse=lambda:False;c.world=World()
 with patch.object(c.follow,'tick_active') as call:c.step();assert call.call_count==1
print('PASS: Cavebot liga com repeticao e volta ao primeiro waypoint; watchdog continua com ataque ativo')
