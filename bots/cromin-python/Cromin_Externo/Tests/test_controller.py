from pathlib import Path
import os,struct,sys,tempfile
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root))
from ui_ipc import Bridge,Position,ATTACK,LOOT,CAVE,HEAL,LOOP
from controller import Controller,StopRequested
import session as session_module

bridge=Bridge()
assert bridge.snapshot().flags==0
with bridge.lock() as s:s.waypoints[0]=Position(100,101,7);s.waypointCount=1
copy=bridge.snapshot();copy.flags=ATTACK;copy.waypoints[0]=Position(1,1,1)
assert bridge.snapshot().flags==0 and bridge.snapshot().waypoints[0]==Position(100,101,7)
owner=session_module.Owner(os.getpid())
try:
    try:duplicate=session_module.Owner(os.getpid())
    except RuntimeError:pass
    else:duplicate.close();raise AssertionError('Duplicate accepted')
finally:owner.close()
print('PASS: estado local isolado, snapshots independentes e somente um controlador por PID')

class Memory:
    def alive(self):pass
class World:
    def __init__(self):self.position={'x':100,'y':100,'z':7}
    def player(self):return dict(self.position)
    def u64(self,address):return 0
    def nearby_corpses(self):return []

with tempfile.TemporaryDirectory(prefix='external_test_',dir=root) as folder:
    assert Path(folder).resolve().is_relative_to((root).resolve())
    now=[0];calls=[];b=Bridge()
    def runner(command,prefix):
        calls.append(command)
        return {'exit_code':0,'report':dict.fromkeys(('started','returned','context_restored','stack_slot_restored','detached'),True)|{'diagnostics':[]}}
    c=Controller(Memory(),0x140000000,1,2,b,folder,runner,lambda:now[0]);c.world=World();hp=[100];c.health=lambda:hp[0]
    c.step();assert not calls
    with b.lock() as s:s.flags=HEAL;s.healText='exura'
    assert not c.heal(b.snapshot());hp[0]=70
    assert c.heal(b.snapshot()) and len(calls)==1
    now[0]=1.9;assert not c.heal(b.snapshot())
    now[0]=2.1;assert c.heal(b.snapshot()) and len(calls)==2
    b.disable();c.step();assert len(calls)==2
    with b.lock() as s:s.flags=CAVE;s.waypointCount=1;s.waypoints[0]=Position(102,100,7)
    c.waypoint(b.snapshot());c.waypoint(b.snapshot());assert len(calls)==3
    c.world.position={'x':102,'y':100,'z':7};c.waypoint(b.snapshot());assert not b.snapshot().flags&CAVE
    with b.lock() as s:s.flags=HEAL|CAVE
    now[0]=5;c.world.position={'x':100,'y':100,'z':7};c.step()
    assert len(calls)==4 and str(calls[-1][0]).endswith('OTC_Talk_Once.exe')
    c.native_busy=True
    try:c.run_native(['never.exe'])
    except RuntimeError:pass
    else:raise AssertionError('Concurrent native action accepted')
    c.native_busy=False;b.close()
    try:c.run_native(['never.exe'])
    except StopRequested:pass
    else:raise AssertionError('Closed UI sent action')
    assert len(calls)==4
print('PASS: modulos desligados, HP/cooldown, prioridade do healing, rota unica, chegada e parada')

# Conexao simulada: o worker confere PID/thread e fecha sua memoria sem carregar qualquer DLL.
originals={name:getattr(session_module,name) for name in ('choose_pid','Memory','verify_build','Controller')}
events=[]
class FakeProcess:
    def __init__(self,pid):events.append(('open',pid))
    def exact(self,address,size):return struct.pack('<I',321)
    def close(self):events.append(('close',))
class FakeController:
    def __init__(self,memory,base,pid,thread,bridge,folder):
        self.quit=False;self.fault=None;self.bridge=bridge;assert thread==321
    def run(self):events.append(('run_flags',self.bridge.snapshot().flags))
try:
    session_module.choose_pid=lambda name,pid:os.getpid()
    session_module.Memory=FakeProcess
    session_module.verify_build=lambda memory:(0x140000000,'test')
    session_module.Controller=FakeController
    test=session_module.Session();test.start();test.worker.join(3)
    assert not test.alive() and not test.error and ('run_flags',0) in events and events[-1]==('close',)
    events.clear();test=session_module.Session();test.stop();test.start();test.worker.join(3)
    assert not test.alive() and ('run_flags',0) not in events and events[-1]==('close',)
finally:
    for name,value in originals.items():setattr(session_module,name,value)
print('PASS: conexao em segundo plano, flags iniciais zero e encerramento antes de iniciar automacoes')
