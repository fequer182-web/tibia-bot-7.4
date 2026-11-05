from pathlib import Path
import struct,sys,tempfile
from unittest.mock import patch
root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'src'))
from cromin.readers.world import WorldReader,WorldSnapshotUnavailable
from cromin.core.controller import Controller,StopRequested
from cromin.core.state import Bridge,ATTACK,LOOT,LIGHT,CAVE
class Memory:
 def __init__(self):self.data={}
 def alloc(self,a,n):self.data.update({a+i:0 for i in range(n)})
 def put(self,a,fmt,*v):self.data.update({a+i:b for i,b in enumerate(struct.pack(fmt,*v))})
 def exact(self,a,n):return bytes(self.data[a+i] for i in range(n))
BASE=0x140000000;m=Memory();h,a,b,p,obj=0x100000,0x200000,0x201000,0x300000,0x400000
for address,size in ((h,0x38),(a,0x38),(b,0x38),(p,0x80),(obj,0x80)):m.alloc(address,size)
m.put(BASE+0xE52F30,'<QQ',h,2);m.put(h+8,'Q',a);m.put(h+0x19,'B',1)
m.put(a,'<QQQ',h,h,b);m.put(a+0x20,'I',42);m.put(a+0x28,'<QQ',obj,obj-16)
m.put(b,'<QQQ',h,a,a);m.put(b+0x20,'I',43)
m.put(BASE+0xE52A48,'Q',p);m.put(p,'Q',BASE+0xAE99E0);m.put(p+0x30,'<IIH',100,100,7)
m.put(obj,'Q',BASE+0xAF61D8);m.put(obj+0x20,'<IIH',101,100,7);m.put(obj+0x40,'I',42)
m.put(obj+0x48,'4s',b'Rat\0');m.put(obj+0x58,'<QQ',3,15);m.put(obj+0x69,'B',100)
w=WorldReader(m,BASE)
for operation in (w.rats_on_floor,lambda:w.tree_find(BASE+0xE52F30,999)):
 try:operation()
 except WorldSnapshotUnavailable as error:assert 'ciclic' in str(error)
 else:raise AssertionError('Partial/inconsistent map accepted')
m.put(b+0x10,'Q',h)
assert [v['id'] for v in w.rats_on_floor()]==[42]
for kind in ('count','sentinel','unexpected_node','null_with_control','null_header'):
 if kind=='count':m.put(BASE+0xE52F38,'Q',8193)
 if kind=='sentinel':m.put(h+0x19,'B',0)
 if kind=='unexpected_node':m.put(b+0x19,'B',1)
 if kind=='null_with_control':m.put(b+0x30,'Q',0x55)
 if kind=='null_header':m.put(BASE+0xE52F30,'Q',0)
 try:w.rats_on_floor()
 except WorldSnapshotUnavailable:pass
 else:raise AssertionError(kind)
 m.put(BASE+0xE52F30,'<QQ',h,2);m.put(h+0x19,'B',1);m.put(b+0x19,'B',0);m.put(b+0x30,'Q',0)
assert len(w.rats_on_floor())==1
with tempfile.TemporaryDirectory(dir=root/'data') as folder:
 bridge=Bridge()
 with bridge.lock() as s:s.flags=ATTACK|LOOT|LIGHT|CAVE
 calls=[];c=Controller(None,BASE,1,2,bridge,folder=folder,runner=lambda *args:calls.append(args))
 c.goal={'started':123,'position':{'x':100,'y':100,'z':7}};goal=c.goal
 attempts=[]
 def step():
  attempts.append(1)
  if len(attempts)<=2:
   m.put(b+0x10,'Q',a);w.rats_on_floor()
  m.put(b+0x10,'Q',h);assert len(w.rats_on_floor())==1
  raise StopRequested()
 c.step=step
 with patch('cromin.core.controller.time.sleep'):c.run()
 assert len(attempts)==3 and c.fault is None and c.goal is goal and not calls
 assert bridge.snapshot().flags==ATTACK|LOOT|LIGHT|CAVE
 log=c.log.read_text();assert log.count('map_scan_skipped')==2 and 'partial_candidates_used' in log
print('PASS: reproduz mapa ciclico/alterado, descarta candidatos parciais e recupera na leitura seguinte; preserva flags, luz e rota sem chamadas')
