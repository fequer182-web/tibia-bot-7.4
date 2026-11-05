from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import Mock,patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
from cromin.readers.ancientia_pending import snapshot,GLOBAL_RVA,VTABLE_RVA


class Memory:
    base=0xAB0000
    obj=0x2C3C1A00
    limit=0x600
    def __init__(self):
        self.calls=[];self.blob=bytearray(0x600)
        struct.pack_into('<I',self.blob,0,self.base+VTABLE_RVA)
        struct.pack_into('<III',self.blob,0xc,32785,32245,6)
        for off in (0x44,0x148):
            name=b'Personagem A';self.blob[off:off+len(name)]=name
            struct.pack_into('<II',self.blob,off+16,len(name),15)
        for off,value in ((0x588,340),(0x590,340),(0x5a8,8811),(0x5b0,9),(0x5c0,217),(0x5c8,245)):
            struct.pack_into('<d',self.blob,off,value)
    def alive(self):pass
    def exact(self,address,size):
        self.calls.append((address,size))
        if address==self.base+GLOBAL_RVA:return struct.pack('<I',self.obj)
        if self.obj<=address and address+size<=self.obj+self.limit:
            return bytes(self.blob[address-self.obj:address-self.obj+size])
        raise RuntimeError('Regiao deixou de ser legivel em 0x2C3C2000')


class Tests(unittest.TestCase):
    def test_panel_reads_required_fields_without_touching_next_page(self):
        memory=Memory();current,blob=snapshot(memory,memory.base)
        self.assertEqual(len(blob),0x5d0)
        self.assertEqual(current['known']['health'],340)
        self.assertEqual(current['names']['0x44'],'Personagem A')
        self.assertEqual(current['maxManaCandidates']['0x5c8'],245)
        self.assertNotIn((memory.obj,0x1000),memory.calls)
        self.assertTrue(all(a+n<=0x2C3C2000 for a,n in memory.calls if a>=memory.obj))

    def test_optional_discovery_stops_at_missing_page_without_zero_fill(self):
        memory=Memory();current,blob=snapshot(memory,memory.base,discovery=True)
        self.assertEqual(len(blob),0x600)
        self.assertIn('0x2C3C2000',current['optionalTailError'])
        self.assertEqual(current['readableObjectBytes'],0x600)

    def test_required_field_failure_is_not_hidden_or_replaced(self):
        memory=Memory();memory.limit=0x580
        with self.assertRaisesRegex(RuntimeError,'legivel'):snapshot(memory,memory.base)

    def test_target_failure_is_labelled_and_not_ignored(self):
        from cromin.readers import ancientia_actions as actions
        memory=Memory()
        original=memory.exact
        target=0x2C3C1FF0
        def exact(address,size):
            if address==memory.base+actions.GAME_RVA+0x34:return struct.pack('<I',target)
            return original(address,size)
        memory.exact=exact
        with self.assertRaisesRegex(RuntimeError,'Leitura do alvo 0x2c3c1ff0'):
            actions.read_state(memory,memory.base)

    def test_failed_panel_connection_closes_memory_handle(self):
        sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
        import ancientia_funcional as panel_module
        panel=panel_module.Panel.__new__(panel_module.Panel)
        panel.pid=Mock();panel.pid.get.return_value='31648'
        panel.disconnect=Mock();panel.guarded=lambda callback:callback()
        memory=Mock()
        with patch.object(panel_module,'Memory',return_value=memory), \
             patch.object(panel_module,'connect',return_value=0xAB0000), \
             patch.object(panel_module,'validate_action_readers'), \
             patch.object(panel_module,'read_state',side_effect=RuntimeError('pagina indisponivel')):
            with self.assertRaisesRegex(RuntimeError,'indisponivel'):panel.connect()
        memory.close.assert_called_once()
        self.assertIsNone(panel.memory)
        self.assertIsNone(panel.runner)


if __name__=='__main__':unittest.main(verbosity=2)
