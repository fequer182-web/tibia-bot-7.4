from pathlib import Path
import sys,tempfile
from unittest.mock import patch
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'src'))
from cromin.core.controller import Controller,StopRequested
from cromin.actions.common import SkipCycle
from cromin.core.state import Bridge,ATTACK,LOOT,LIGHT
from cromin.actions.combat_cycle import cycle
from cromin.actions.open_loot import pick_corpse

class World:
    def player(self):return {'x':100,'y':100,'z':7}
    def rats_on_floor(self):return [{'id':42,'object':'0x12345','position':self.player(),'hp':100}]
    def nearby_corpses(self,*args):return []
    def rat(self,rid):return None
    def corpses_at(self,p):raise AssertionError('Must not open or inspect an uncertain death tile')
calls=[];report={}
def runner(command,prefix):
    calls.append(command)
    return {'exit_code':0,'report':dict.fromkeys(('started','returned','context_restored',
        'stack_slot_restored','detached','attack_requested','attack_returned','owned_reference_handled'),True)|
        {'rat_id':42,'rat_object':'0x12345','rat_position_at_request':{'x':105,'y':100,'z':7},'diagnostics':[]}}
try:
    with patch('cromin.actions.combat_cycle.time.sleep'):
        cycle(None,World(),1,2,0,1,report,lambda:False,runner,collect=lambda:False)
except SkipCycle as error:assert 'Ultimo tile' in str(error)
else:raise AssertionError('Unsurveyed tile accepted')
assert len(calls)==1 and 'new_corpse' not in report
try:pick_corpse(World())
except SkipCycle:pass
else:raise AssertionError('Missing corpse accepted')

with tempfile.TemporaryDirectory(dir=root) as folder:
    bridge=Bridge()
    with bridge.lock() as s:s.flags=ATTACK|LOOT|LIGHT
    controller=Controller(None,0,1,2,bridge,folder=folder)
    attempts=[]
    def step():
        attempts.append(1)
        if len(attempts)==1:raise SkipCycle('Ultimo tile nao observado')
        raise StopRequested()
    controller.step=step
    with patch('cromin.core.controller.time.sleep'):controller.run()
    assert len(attempts)==2 and controller.fault is None
    assert bridge.snapshot().flags==ATTACK|LOOT|LIGHT
    assert 'cycle_skipped' in controller.log.read_text()
    controller.step=lambda:(_ for _ in ()).throw(RuntimeError('Chamada/restauracao nao confirmada'))
    def after_fault():
        if controller.fault:controller.quit=True
    with patch('cromin.core.controller.time.sleep',side_effect=lambda *_:after_fault()):controller.run()
    assert controller.fault and bridge.snapshot().flags==0
print('PASS: erro do tile ignora somente o ciclo, mantem flags e continua; falha nativa ainda interrompe')
