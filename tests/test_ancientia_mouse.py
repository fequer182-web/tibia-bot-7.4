from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
from cromin.actions.ancientia_mouse import project
from cromin.readers.ancientia_actions import scan_monsters,MONSTER_VTABLE_RVA
from cromin.core.ancientia_runner import Runner


class Tests(unittest.TestCase):
    def test_projection_and_refusal(self):
        cal=dict(rect=[0,0,800,600],center=[400,300],tile=32)
        self.assertEqual(project(cal,(100,100,7),(102,99,7),cal['rect']),(464,268))
        for monster,rect in (((100,100,8),cal['rect']),((100,100,7),cal['rect']),
                             ((110,100,7),cal['rect']),((101,100,7),[0,0,900,600])):
            with self.assertRaises(ValueError):project(cal,(100,100,7),monster,rect)

    def test_scan_valid_monster_and_reject_dead_or_bad_position(self):
        base,heap=0xAB0000,0x30000000
        data=bytearray(4096)
        for off,hp,x in ((0x100,90,102),(0x300,0,103),(0x500,50,65535)):
            struct.pack_into('<I',data,off,base+MONSTER_VTABLE_RVA)
            struct.pack_into('<III',data,off+0xc,x,100,7)
            data[off+0x44:off+0x47]=b'Rat'
            struct.pack_into('<II',data,off+0x54,3,15)
            data[off+0x5c]=hp
        class Memory:
            def regions(self,scope):yield heap,len(data)
            def exact(self,address,size):
                if heap<=address and address+size<=heap+len(data):return bytes(data[address-heap:address-heap+size])
                raise RuntimeError('fora da pagina')
            def alive(self):pass
        found,stats=scan_monsters(Memory(),base)
        self.assertEqual(len(found),1);self.assertEqual(found[0]['name'],'Rat')
        self.assertEqual(found[0]['position'],(102,100,7))
        found,stats=scan_monsters(Memory(),base,cancel=lambda:True)
        self.assertEqual(found,[]);self.assertTrue(stats['cancelled'])

    def test_mouse_runner_confirms_exact_target_and_pauses_for_other(self):
        calls=[]
        runner=Runner(lambda key:False,attack_action=lambda:calls.append(1) or {'address':'0x30000100'})
        runner.attack=True
        state=dict(health=100,position=(100,100,7),target=None)
        runner.tick(state,False,0);self.assertEqual(calls,[])
        runner.tick(state,True,1);self.assertEqual(calls,[1])
        state['target']=dict(className='Monster',hpPercent=90,address='0x30000200')
        runner.tick(state,True,1.5)
        self.assertFalse(runner.attack)
        self.assertIn('difere',runner.status)


if __name__=='__main__':unittest.main(verbosity=2)
