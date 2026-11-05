from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
from cromin.actions.ancientia_light import AncientiaLight
from cromin.readers.ancientia_pending import GLOBAL_RVA,VTABLE_RVA


class Tests(unittest.TestCase):
    def make(self):
        allowed=patch('cromin.actions.ancientia_light.LIGHT_WRITE_ALLOWED',True)
        allowed.start();self.addCleanup(allowed.stop)
        base,obj=0xAB0000,0x30000000
        class Memory:
            level=7;name='Personagem B';vt=base+VTABLE_RVA
            def exact(self,address,size):
                if address==base+GLOBAL_RVA:return struct.pack('<I',obj)
                if address==obj:return struct.pack('<I',self.vt)
                if address==obj+0xc9:return bytes([self.level])
                if address==obj+0x44:
                    data=bytearray(24);b=self.name.encode();data[:len(b)]=b
                    struct.pack_into('<II',data,16,len(b),15);return bytes(data)
                raise RuntimeError('Endereco inesperado')
        memory=Memory();writes=[];records=[]
        class Writer:
            closed=False
            def write(self,address,value):
                assert address==obj+0xC9
                writes.append((address,value));memory.level=value
            def close(self):self.closed=True
        writer=Writer()
        light=AncientiaLight(memory,base,1,lambda e,d:records.append((e,d)),lambda pid:writer)
        return light,memory,writes,records,writer

    def test_apply_restore_only_intensity_and_correct_log(self):
        light,memory,writes,records,writer=self.make()
        light.tick(True,31);self.assertEqual(memory.level,31)
        self.assertEqual(records[0][1]['offset'],'0xC9')
        self.assertEqual(light.disable(),'restored')
        self.assertEqual(memory.level,7);self.assertTrue(writer.closed)
        self.assertEqual([v for a,v in writes],[31,7])

    def test_client_update_preserved_when_disabled(self):
        light,memory,writes,_,_=self.make()
        light.tick(True,31);memory.level=0
        self.assertEqual(light.disable(),'client_value_preserved')
        self.assertEqual(memory.level,0);self.assertEqual(len(writes),1)

    def test_same_address_new_character_does_not_receive_old_restore(self):
        light,memory,writes,_,_=self.make()
        light.tick(True,31);memory.name='Outro';memory.level=0
        self.assertEqual(light.disable(),'restore_skipped_player_unavailable')
        self.assertEqual(len(writes),1)
        light.tick(True,31);light.disable();self.assertEqual(memory.level,0)

    def test_invalid_vtable_or_level_writes_nothing(self):
        for field,value in [('level',200),('vt',0)]:
            light,memory,writes,_,_=self.make();setattr(memory,field,value)
            with self.assertRaises(ValueError):light.tick(True,31)
            self.assertEqual(writes,[])

    def test_new_build_light_remains_blocked_until_validated(self):
        light,memory,writes,_,_=self.make()
        with patch('cromin.actions.ancientia_light.LIGHT_WRITE_ALLOWED',False):
            with self.assertRaisesRegex(ValueError,'bloqueado'):light.tick(True,31)
        self.assertEqual(writes,[])


if __name__=='__main__':unittest.main(verbosity=2)
