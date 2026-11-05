from pathlib import Path
import sys
import threading
import unittest
from unittest.mock import Mock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from ancientia_funcional import Panel
from cromin.core.ancientia_runner import Runner


class Variable:
    def __init__(self,value):self.value=value
    def get(self):return self.value
    def set(self,value):self.value=value


class Tests(unittest.TestCase):
    def panel(self):
        panel=Panel.__new__(Panel)
        panel.runner=Runner(Mock(return_value=True))
        panel.attack_mode=Variable('Hotkey');panel.attack_key=Variable('TAB')
        panel.hotkey_confirmed=Variable(True);panel.status=Variable('')
        panel.calibration=None;panel.scan_cancel=threading.Event()
        panel.need_connection=Mock();panel.request_scan=Mock();panel.record=Mock()
        panel.guarded=lambda fn:fn()
        panel.hotkey_frame=Mock();panel.mouse_frame=Mock()
        panel.click_monster=Mock(return_value=False)
        return panel

    def test_hotkey_needs_no_calibration_or_monster_scan(self):
        p=self.panel();p.start_attack()
        self.assertTrue(p.runner.attack)
        self.assertEqual(p.runner.attack_key,'TAB')
        self.assertIsNone(p.runner.attack_action)
        p.request_scan.assert_not_called()
        self.assertTrue(p.scan_cancel.is_set())
        state=dict(health=100,position=(100,100,7),target=None)
        p.runner.tick(state,True,0)
        p.runner.send.assert_called_once_with('TAB')
        p.click_monster.assert_not_called()

    def test_unconfirmed_or_stop_key_refused(self):
        p=self.panel();p.hotkey_confirmed.set(False)
        with self.assertRaises(ValueError):p.start_attack()
        self.assertFalse(p.runner.attack)
        p.hotkey_confirmed.set(True);p.attack_key.set('F8')
        with self.assertRaises(ValueError):p.start_attack()
        self.assertFalse(p.runner.attack)

    def test_mouse_optional_still_requires_calibration(self):
        p=self.panel();p.attack_mode.set('Mouse')
        with self.assertRaises(ValueError):p.start_attack()
        p.calibration={'tile':32};p.start_attack()
        self.assertTrue(p.runner.attack)
        self.assertIs(p.runner.attack_action,p.click_monster)
        p.request_scan.assert_called_once()

    def test_mode_or_key_change_disarms_attack_and_preserves_cave(self):
        p=self.panel();p.runner.attack=True;p.runner.cave=True;p.runner.pending_attack=1
        p.change_attack_mode()
        self.assertFalse(p.runner.attack);self.assertTrue(p.runner.cave)
        self.assertIsNone(p.runner.pending_attack)
        p.hotkey_frame.pack.assert_called_once()
        p.runner.attack=True;p.change_attack_key()
        self.assertFalse(p.runner.attack);self.assertFalse(p.hotkey_confirmed.get())


if __name__=='__main__':unittest.main(verbosity=2)
