from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
from cromin.readers import ancientia_pending as pending


class Tests(unittest.TestCase):
    def test_max_mana_separated_from_percent_without_level_change(self):
        data=bytearray(0x1000)
        struct.pack_into('<d',data,0x5b8,5)
        struct.pack_into('<d',data,0x5c8,5)
        previous=pending.refine_field(None,data,'maxMana',5)
        self.assertEqual(previous['candidateCount'],2)
        struct.pack_into('<d',data,0x5b8,10)
        maximum=pending.refine_field(previous,data,'maxMana',5)
        self.assertEqual(maximum['candidates'],[dict(offset='0x5c8',type='double',storedScale=1)])
        self.assertEqual(maximum['status'],'unique_snapshot_only')
        # Mana maxima nao mudou: unicidade nao e transicao desse campo.
        self.assertEqual(maximum['transitions'],0)
        percent=pending.refine_field(None,data,'levelPercent',10)
        struct.pack_into('<d',data,0x5b8,15)
        percent=pending.refine_field(percent,data,'levelPercent',15)
        self.assertEqual(percent['status'],'unique_with_transition')
        self.assertEqual(percent['candidates'][0]['offset'],'0x5b8')

    def test_known_fields_not_promoted_as_new_attribute(self):
        data=bytearray(0x1000)
        struct.pack_into('<d',data,0x5c0,5)
        self.assertEqual(pending.candidate_matches(data,'maxMana',5),[])

    def test_capacity_scale_decimal(self):
        data=bytearray(0x1000)
        struct.pack_into('<d',data,0x598,31620)
        self.assertIn(dict(offset='0x598',type='double',storedScale=100),
                      pending.candidate_matches(data,'capacity',316.2))

    def test_zero_too_broad_then_nonzero_can_seed(self):
        data=bytearray(0x1000)
        state=pending.refine_field(None,data,'maxMana',0)
        self.assertEqual(state['status'],'too_many_candidates')
        self.assertEqual(state['candidates'],[])
        struct.pack_into('<d',data,0x5c8,5)
        state=pending.refine_field(state,data,'maxMana',5)
        self.assertEqual(state['candidateCount'],1)
        self.assertEqual(state['status'],'unique_snapshot_only')

    def test_snapshot_resolves_global_each_time_and_rejects_switch(self):
        base,obj=0xAB0000,0x30000000
        blob=bytearray(0x1000)
        struct.pack_into('<I',blob,0,base+pending.VTABLE_RVA)
        for key,off in pending.KNOWN.items():
            struct.pack_into('<d',blob,off,{'health':98,'maxHealth':155,'mana':5,'experience':105,'level':2}[key])
        for off in (0x44,0x148):
            name=b'Personagem B'
            blob[off:off+len(name)]=name
            struct.pack_into('<II',blob,off+16,len(name),15)
        struct.pack_into('<III',blob,0xc,32096,32216,7)
        class Memory:
            changed=False
            globals=0
            def alive(self): pass
            def exact(self,address,size):
                if address==base+pending.GLOBAL_RVA:
                    self.globals+=1
                    return struct.pack('<I',obj+(4 if self.changed and self.globals>1 else 0))
                if obj<=address and address+size<=obj+len(blob):
                    return bytes(blob[address-obj:address-obj+size])
                raise RuntimeError('inacessivel')
        current,_=pending.snapshot(Memory(),base)
        self.assertEqual(current['names']['0x44'],'Personagem B')
        self.assertEqual(current['known']['mana'],5)
        m=Memory();m.changed=True
        with self.assertRaisesRegex(RuntimeError,'mudou'):
            pending.snapshot(m,base)


if __name__=='__main__':
    unittest.main(verbosity=2)
