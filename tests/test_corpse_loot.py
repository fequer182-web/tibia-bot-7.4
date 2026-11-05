import sys
from pathlib import Path
import unittest
from unittest.mock import Mock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
from cromin.core.ancientia_loot import CorpseLoot
class Tests(unittest.TestCase):
 def setUp(self):
  self.dead=dict(address='0x20000',name='rat',position=(101,100,7),hpPercent=0,className='Monster')
  self.bags=Mock(return_value=[dict(containerId=0,address='0x30000')])
  self.click=Mock(return_value=True)
  self.inspect=Mock(return_value=self.dead)
  self.loot=CorpseLoot(self.inspect,self.bags,self.click);self.loot.enabled=True
  self.sample=dict(health=100,position=(100,100,7),target=dict(self.dead,hpPercent=10))
  self.loot.tick(self.sample,True,0);self.sample['target']=None
 def test_click_then_confirm(self):
  self.assertTrue(self.loot.tick(self.sample,True,1));self.click.assert_called_once()
  self.bags.return_value.append(dict(containerId=1,address='0x40000'))
  self.assertTrue(self.loot.tick(self.sample,True,1.5));self.assertIsNone(self.loot.pending)
  self.loot.tick(self.sample,True,2);self.click.assert_called_once()
 def test_disappearance_waits_then_attempts(self):
  self.inspect.return_value=dict(self.dead,hpPercent=10)
  self.assertTrue(self.loot.tick(self.sample,True,1));self.click.assert_not_called()
  self.assertTrue(self.loot.tick(self.sample,True,1.7));self.click.assert_called_once()
 def test_new_target_cancels_disappearance_wait(self):
  self.inspect.return_value=None
  self.loot.tick(self.sample,True,1)
  self.sample['target']=dict(self.dead,address='0x25000',hpPercent=80)
  self.loot.tick(self.sample,True,2);self.click.assert_not_called()
  self.assertIsNone(self.loot.lost_at)
 def test_background_wait(self):
  self.assertTrue(self.loot.tick(self.sample,False,1));self.click.assert_not_called()
 def test_distant_skip(self):
  self.inspect.return_value=dict(self.dead,position=(105,100,7))
  self.loot.tick(self.sample,True,1);self.click.assert_not_called()
 def test_missing_bag_disables(self):
  self.bags.return_value=[];self.loot.tick(self.sample,True,1)
  self.assertFalse(self.loot.enabled);self.click.assert_not_called()
 def test_timeout_no_repeat(self):
  self.loot.tick(self.sample,True,1)
  for i in range(1,8):self.loot.tick(self.sample,True,1+i*0.5)
  self.loot.tick(self.sample,True,8);self.loot.tick(self.sample,True,9)
  self.assertEqual(self.click.call_count,8)
  self.assertEqual(len({tuple(c.args[1]) for c in self.click.call_args_list}),8)
 def test_motion_cancels_scan(self):
  self.loot.tick(self.sample,True,1);self.sample['position']=(101,100,7)
  self.loot.tick(self.sample,True,2);self.click.assert_called_once()
 def test_manual_sweep_without_previous_target(self):
  self.loot.reset();self.loot.sweep_here(self.sample,1)
  self.loot.tick(self.sample,True,1.1);self.click.assert_called_once()
 def test_stop_clears(self):
  self.loot.tick(self.sample,True,1);self.loot.reset()
  self.assertFalse(self.loot.tick(self.sample,True,2));self.assertIsNone(self.loot.pending)
if __name__=='__main__':unittest.main()
