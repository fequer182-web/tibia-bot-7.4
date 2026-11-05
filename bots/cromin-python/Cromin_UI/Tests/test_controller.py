from pathlib import Path
import contextlib,ctypes as C,os,sys,tempfile
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root))
from ui_ipc import State,Position,Bridge,ATTACK,LOOT,CAVE,HEAL,LOOP
from controller import Controller,StopRequested

class FakeBridge:
    def __init__(self):
        self.data=State();self.data.hpThreshold=80;self.data.healCooldownMs=2000;self.data.healText='exura'
    @contextlib.contextmanager
    def lock(self):yield self.data
    def snapshot(self):return State.from_buffer_copy(self.data)
    def status(self,*args):pass
    def disable(self,bits=15):self.data.flags&=~bits
class FakeWorld:
    def __init__(self):self.position={'x':100,'y':100,'z':7}
    def player(self):return dict(self.position)
    def u64(self,address):return 0
    def nearby_corpses(self):return []
class FakeMemory:
    def alive(self):pass

assert C.sizeof(State)==2048 and State.waypoints.offset==1280
ipc=Bridge(os.getpid())
try:
    assert ipc.snapshot().flags==0 and ipc.snapshot().hpThreshold==80
    with ipc.lock() as s:s.flags=HEAL;s.healText='oi'
    assert ipc.snapshot().healText=='oi'
    try:duplicate=Bridge(os.getpid())
    except RuntimeError:pass
    else:duplicate.close();raise AssertionError('Duplicate worker accepted')
    ipc.disable();assert ipc.snapshot().flags==0
finally:ipc.close()
print('PASS: IPC real de 2048 bytes, offsets, configuracao e controlador unico')

with tempfile.TemporaryDirectory(prefix='cromin_ui_controller_',dir=root) as temp:
    assert Path(temp).resolve().is_relative_to((root).resolve())
    now=[0.0];calls=[];b=FakeBridge()
    def runner(command,prefix):
        calls.append(command)
        return {'exit_code':0,'report':dict.fromkeys(('started','returned','context_restored','stack_slot_restored','detached'),True)|{'diagnostics':[]}}
    controller=Controller(FakeMemory(),0x140000000,1,2,b,temp,runner,lambda:now[0])
    controller.world=FakeWorld();hp=[100];controller.health=lambda:hp[0]
    b.data.flags=HEAL
    assert not controller.heal(b.snapshot()) and not calls
    hp[0]=70;assert controller.heal(b.snapshot()) and len(calls)==1
    now[0]=1.9;assert not controller.heal(b.snapshot()) and len(calls)==1
    now[0]=2.1;assert controller.heal(b.snapshot()) and len(calls)==2
    b.data.flags=0;now[0]=9;controller.step();assert len(calls)==2
    print('PASS: healing somente abaixo do limite absoluto, intervalo e modulos desligados')
    b.data.flags=CAVE;b.data.waypointCount=1;b.data.waypoints[0]=Position(102,100,7)
    controller.waypoint(b.snapshot());assert len(calls)==3 and controller.goal
    controller.waypoint(b.snapshot());assert len(calls)==3
    controller.world.position={'x':102,'y':100,'z':7};controller.waypoint(b.snapshot())
    assert not b.data.flags&CAVE and controller.goal is None
    b.data.flags=CAVE|LOOP;controller.waypoint(b.snapshot());assert b.data.flags&CAVE
    print('PASS: waypoint enviado uma vez, chegada observada e repeticao configuravel')
    b.data.flags=CAVE|HEAL;controller.world.position={'x':100,'y':100,'z':7};controller.goal=None
    now[0]=20;controller.step();assert len(calls)==4 and str(calls[-1][0]).endswith('OTC_Talk_Once.exe')
    b.data.addSerial+=1;b.data.flags=0;controller.commands(b.snapshot());assert b.data.waypointCount==2
    b.data.clearSerial+=1;controller.commands(b.snapshot());assert b.data.waypointCount==0
    controller.native_busy=True
    try:controller.run_native(['never.exe'])
    except RuntimeError:pass
    else:raise AssertionError('Concurrent native action accepted')
    controller.native_busy=False;b.data.uiClosed=1
    try:controller.run_native(['never.exe'])
    except StopRequested:pass
    else:raise AssertionError('Closed UI requested action')
    assert len(calls)==4
    print('PASS: healing tem prioridade sobre cavebot, edicao da rota e nenhuma chamada concorrente/apos fechar')
