from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
from cromin.core.ancientia_runner import Runner
from cromin.readers.ancientia_actions import light_candidates,validate_action_readers,READER_SIGNATURES


def sample(position=(100,100,7),target=None):
    return dict(health=100,position=position,target=target)


class Tests(unittest.TestCase):
    def runner(self):
        keys=[]
        r=Runner(lambda key:keys.append(key) or True)
        return r,keys

    def test_step_readback_and_route_completion(self):
        r,keys=self.runner();r.route=[(101,100,7)];r.cave=True
        r.tick(sample(),True,0)
        self.assertEqual(keys,['RIGHT'])
        r.tick(sample(),True,.3)
        self.assertEqual(len(keys),1)
        r.tick(sample((101,100,7)),True,.7)
        self.assertFalse(r.cave)
        self.assertEqual(r.status,'Rota concluida')

    def test_no_focus_or_unconfirmed_send_does_not_queue_step(self):
        r,keys=self.runner();r.route=[(101,100,7)];r.cave=True
        r.tick(sample(),False,0);self.assertEqual(keys,[])
        r.send=lambda key:False
        r.tick(sample(),True,1);self.assertIsNone(r.pending_step)

    def test_blocked_route_stops_without_flood(self):
        r,keys=self.runner();r.route=[(101,100,7)];r.cave=True
        for t in (0,2,3,5,6,8):r.tick(sample(),True,t)
        self.assertFalse(r.cave);self.assertEqual(len(keys),3)

    def test_wrong_floor_distance_or_death_stops(self):
        for dest in ((100,100,8),(200,100,7)):
            r,keys=self.runner();r.route=[dest];r.cave=True
            r.tick(sample(),True,0)
            self.assertFalse(r.cave);self.assertEqual(keys,[])
        r,keys=self.runner();r.attack=True
        r.tick(dict(sample(),health=0),True,0)
        self.assertFalse(r.attack);self.assertEqual(keys,[])

    def test_attack_confirmation_pauses_cave_and_nonmonster_stops(self):
        r,keys=self.runner();r.attack=True;r.cave=True;r.route=[(101,100,7)]
        r.tick(sample(),True,0)
        self.assertEqual(keys,['SPACE'])
        monster=dict(className='Monster',hpPercent=90)
        r.tick(sample(target=monster),True,.5)
        self.assertIsNone(r.pending_attack)
        r.tick(sample(target=monster),True,1)
        self.assertEqual(keys,['SPACE'])
        r.tick(sample(target=dict(className='Other',hpPercent=90)),True,2)
        self.assertFalse(r.attack);self.assertFalse(r.cave)
        self.assertEqual(keys,['SPACE'])

    def test_empty_target_does_not_prevent_cave_forever(self):
        r,keys=self.runner();r.attack=True;r.cave=True;r.route=[(101,100,7)]
        r.tick(sample(),True,0)
        r.tick(sample(),True,1.6)
        self.assertEqual(keys,['SPACE','RIGHT'])

    def test_stop_clears_pending_operations(self):
        r,keys=self.runner();r.attack=True;r.cave=True;r.pending_attack=0
        r.pending_step=((100,100,7),0)
        r.stop()
        self.assertFalse(r.attack or r.cave)
        self.assertIsNone(r.pending_attack);self.assertIsNone(r.pending_step)

    def test_light_repeated_transition_and_noise(self):
        states=[]
        for i,value in enumerate((0,7,0,7)):
            first=bytearray(0x500);first[0xe0]=value;first[0xe1]=215 if value else 0
            first[0x100]=i
            second=bytearray(first);second[0x200]=i+1
            states.append(dict(player='0x30000000',position=[100,100,7],first=first.hex(),second=second.hex()))
        hits=light_candidates(states)
        self.assertEqual([c['offset'] for c in hits],['0xe0','0xe1'])
        self.assertTrue(hits[0]['levelCandidate']);self.assertFalse(hits[1]['levelCandidate'])
        states[-1]['position']=[101,100,7]
        with self.assertRaises(ValueError):light_candidates(states)

    def test_action_reader_signatures_refuse_changed_body(self):
        base=0xAB0000
        bodies={base+rva:bytes.fromhex(code) for rva,code in READER_SIGNATURES}
        class Memory:
            def exact(self,address,size):return bodies[address]
        validate_action_readers(Memory(),base)
        bodies[base+READER_SIGNATURES[0][0]]=b'\0'*3
        with self.assertRaises(RuntimeError):validate_action_readers(Memory(),base)


if __name__=='__main__':unittest.main(verbosity=2)
