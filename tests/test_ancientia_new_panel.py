from pathlib import Path
import sys
import unittest
from unittest.mock import Mock,patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import ancientia_funcional as app


class Tests(unittest.TestCase):
    def panel(self):
        p=app.Panel.__new__(app.Panel);p.expected_name='Personagem A'
        p.pid=Mock();p.pid.get.return_value='22336';p.disconnect=Mock()
        p.guarded=lambda callback:callback();p.status=Mock();p.record=Mock()
        return p

    def test_other_character_refused_before_input_backend(self):
        p=self.panel();memory=Mock()
        with patch.object(app,'Memory',return_value=memory),patch.object(app,'connect',return_value=0x960000), \
             patch.object(app,'validate_action_readers'),patch.object(app,'read_state',return_value={'name':'Personagem B','player':'0x10000'}), \
             patch.object(app,'Mouse') as mouse:
            with self.assertRaisesRegex(ValueError,'Personagem B'):p.connect()
            mouse.assert_not_called()
        memory.close.assert_called_once()
        self.assertIsNone(p.memory)

    def test_selected_character_connects_with_automation_off(self):
        p=self.panel();memory=Mock()
        with patch.object(app,'Memory',return_value=memory),patch.object(app,'connect',return_value=0x960000), \
             patch.object(app,'validate_action_readers'),patch.object(app,'read_state',return_value={'name':'Personagem A','player':'0x27d7e038'}), \
             patch.object(app,'Mouse') as mouse,patch.object(app,'AncientiaLight') as light:
            p.connect();self.assertIs(p.memory,memory)
            self.assertFalse(p.runner.attack or p.runner.cave)
            mouse.return_value.send.assert_not_called();light.return_value.tick.assert_not_called()


if __name__=='__main__':unittest.main(verbosity=2)
