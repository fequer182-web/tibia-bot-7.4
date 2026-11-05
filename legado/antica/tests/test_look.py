from pathlib import Path
import json,sys,tempfile,threading
root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'src'))
from cromin.core.look_observer import LookObserver
from cromin.core.state import Bridge
from cromin.core.controller import Controller
from cromin.ui.panel import footer_text
class Event:
 def __init__(self):self.name='fixture';self.event=threading.Event();self.closed=False
 def set(self):self.event.set()
 def close(self):self.closed=True
class Output:
 def __init__(self,process,cancel,clean,preflight=False):self.process,self.cancel,self.clean,self.preflight=process,cancel,clean,preflight;self.closed=False
 def __iter__(self):
  if self.preflight:
   yield '[ERRO] preflight fixture\n';self.process.code=1;return
  yield json.dumps({'event':'ready'})+'\n'
  yield json.dumps({'event':'look','kind':'item','id':3994,'object':'0x1234','position':{'x':100,'y':101,'z':7}})+'\n'
  self.process.received.set()
  if not self.cancel.event.wait(3):raise RuntimeError('Stop event not signalled')
  yield json.dumps({'event':'closed','clean':self.clean,'detached':self.clean})+'\n'
  self.process.code=0
 def close(self):self.closed=True
class Process:
 def __init__(self,cancel,clean=True,preflight=False):
  self.code=None;self.received=threading.Event();self.stdout=Output(self,cancel,clean,preflight)
 def poll(self):return self.code
 def wait(self,timeout=None):assert self.code is not None;return self.code

def observer(bridge,events,clean=True,preflight=False):
 cancel=Event();process=Process(cancel,clean,preflight)
 watch=LookObserver(1,bridge,lambda *args:events.append(args),event_factory=lambda:cancel,launcher=lambda *args,**kwargs:process)
 return watch,cancel,process
bridge=Bridge();events=[];watch,cancel,process=observer(bridge,events)
watch.start();assert process.received.wait(2)
assert bridge.snapshot().lastLookId==3994 and bridge.snapshot().lastLookKind=='item'
assert footer_text(bridge.snapshot(),29564,True)=='Online PID: 29564 · ID: 3994'
for bad in ({'event':'look','kind':'item','id':True},{'event':'look','kind':'item','id':65536},{'event':'look','kind':'other','id':12}):watch.message(bad)
assert bridge.snapshot().lastLookId==3994
watch.message({'event':'look','kind':'monster','id':0x40000001});assert bridge.snapshot().lastLookId==0
assert '3994' not in footer_text(bridge.snapshot(),29564,True)
watch.message({'event':'look','kind':'item','id':3031});assert bridge.snapshot().lastLookId==3031
with tempfile.TemporaryDirectory(dir=root/'data') as folder:
 calls=[]
 def native(*args):assert watch.process is None and cancel.closed;calls.append(args);return {'exit_code':0,'report':{}}
 controller=Controller(None,0,1,2,bridge,folder=folder,runner=native);controller.look=watch
 controller.run_native(['fixture'],'[JSON] ')
 assert len(calls)==1 and not controller.native_busy and watch.process is None
 assert any(event[0]=='manual_look' for event in events)
 # The next pulse can resume observation; it is not restarted inside a native helper.
 states=[]
 class Monitor:
  def start(self):states.append('start')
  def close(self):states.append('close')
 controller.look=Monitor();controller.memory=type('M',(),{'alive':lambda self:None})()
 controller.world=type('W',(),{'player':lambda self:{'x':100,'y':100,'z':7}})();controller.health=lambda:100
 controller.pulse();assert states==['start']
 controller.close();assert states==['start','close']
 bridge2=Bridge();bad,stop2,proc2=observer(bridge2,[],clean=False);bad.start();assert proc2.received.wait(2)
 controller.look=bad
 try:controller.run_native(['must_not_run'])
 except RuntimeError:pass
 else:raise AssertionError('Native action started without detach confirmation')
 assert len(calls)==1
 unavailable,stop3,proc3=observer(Bridge(),[],preflight=True);unavailable.start();assert unavailable.done.wait(2)
 unavailable.pause();assert unavailable.disabled and not unavailable.critical and stop3.closed
print('PASS: Look item ID no estado, nao confunde criatura com item, restaura antes de acao, retoma no pulse e bloqueia detach inseguro')
