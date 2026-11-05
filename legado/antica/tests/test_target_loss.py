from pathlib import Path
import runpy,sys
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'src'))
fixtures={'__file__':str(root/'tests/test_cycle.py')}
source=(root/'tests/test_cycle.py').read_text().replace("outputs/OTC_Rat_Cycle","outputs/Cromin_Externo")
exec(compile(source,fixtures['__file__'],'exec'),fixtures)
from cromin.actions.combat_cycle import cycle
from cromin.readers.world import RatDataError
World,Memory,result,POSITION=[fixtures[k] for k in ('World','Memory','result','POSITION')]

class Lost(World):
    def __init__(self,change):super().__init__();self.change=change
    def rat(self,rid):
        raise RatDataError(dict(id=42,actual_id=42,object='0x987654',hp=100,
            position={'x':65535,'y':65535,'z':255},reason='invalid_position_or_health')|self.change)

for change in ({},{'object':'0x111111'},{'actual_id':43},{'hp':101},
               {'position':{'x':100,'y':100,'z':255}},{'reason':'pointer_pair_changed_or_null_control'}):
    calls=[];report={}
    def runner(command,prefix):
        calls.append(Path(command[0]).name)
        assert len(calls)==1 and calls[0]=='OTC_Rat_Attack_Once.exe'
        return result(attack_requested=True,attack_returned=True,owned_reference_handled=True,
            rat_id=42,rat_object='0x987654',rat_position_at_request=POSITION)
    try:cycle(Memory(),Lost(change),123,456,0,1,report,lambda:False,runner)
    except RatDataError:assert change
    else:
        assert not change and report['status']=='target_lost'
        assert report['loot_skipped'] and not report['confirmed'] and report['collected']==0
        assert 'new_corpse' not in report
    assert len(calls)==1
print('PASS: perda de alvo vivo recuperavel, sem abrir/saquear; identidade e dados corrompidos continuam rejeitados')
