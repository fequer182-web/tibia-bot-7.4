import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
from cromin.readers.ancientia_loot import inventory, GAME_RVA, CONTAINER_VT, ITEM_VT

class Memory:
    base = 0x960000
    def __init__(self):
        self.blocks = {}
        self.put(self.base + GAME_RVA + 0x40, struct.pack('<II', 0x20000, 1))
        head = bytearray(16); struct.pack_into('<III', head, 0, 0x21000, 0x21000, 0x21000); head[13] = 1
        self.put(0x20000, head)
        node = bytearray(24); struct.pack_into('<III', node, 0, 0x20000, 0x20000, 0x20000)
        struct.pack_into('<iI', node, 16, 2, 0x22000); self.put(0x21000, node)
        bag = bytearray(0x50); struct.pack_into('<I', bag, 0, self.base + CONTAINER_VT)
        struct.pack_into('<II', bag, 0xC, 2, 20)
        # Offset 31 wraps from map[7], element 3 to map[0], element 0.
        struct.pack_into('<IIII', bag, 0x40, 0x23000, 8, 31, 2); self.put(0x22000, bag)
        table = bytearray(32); struct.pack_into('<I', table, 0, 0x24000); struct.pack_into('<I', table, 28, 0x25000)
        self.put(0x23000, table); self.put(0x24000, struct.pack('<4I', 0x27000, 0, 0, 0))
        self.put(0x25000, struct.pack('<4I', 0, 0, 0, 0x26000))
        for address, ident in ((0x26000, 3031), (0x27000, 3492)):
            item = bytearray(0x62); struct.pack_into('<I', item, 0, self.base + ITEM_VT)
            struct.pack_into('<H', item, 0x3D, ident); struct.pack_into('<H', item, 0x60, 7)
            self.put(address, item)
    def put(self, address, data): self.blocks[address] = bytearray(data)
    def exact(self, address, size):
        for start, data in self.blocks.items():
            if start <= address and address + size <= start + len(data):
                return bytes(data[address-start:address-start+size])
        raise RuntimeError('Endereco ausente')

class LootTests(unittest.TestCase):
    def test_deque_wrap_and_unaligned_item_id(self):
        memory = Memory(); bags = inventory(memory, memory.base)
        self.assertEqual([i['itemId'] for i in bags[0]['items']], [3031, 3492])
        self.assertNotIn('quantity', bags[0]['items'][0])
    def test_cycle_rejected(self):
        memory = Memory(); struct.pack_into('<I', memory.blocks[0x21000], 0, 0x21000)
        with self.assertRaises(RuntimeError): inventory(memory, memory.base)
    def test_null_registry_value_is_not_open_container(self):
        memory = Memory(); struct.pack_into('<I', memory.blocks[0x21000], 20, 0)
        self.assertEqual(inventory(memory, memory.base), [])
    def test_wrong_class_rejected(self):
        memory = Memory(); struct.pack_into('<I', memory.blocks[0x26000], 0, 0)
        with self.assertRaises(RuntimeError): inventory(memory, memory.base)
    def test_invalid_deque_rejected(self):
        memory = Memory(); struct.pack_into('<I', memory.blocks[0x22000], 0x44, 7)
        with self.assertRaises(RuntimeError): inventory(memory, memory.base)
    def test_container_identity_rejected(self):
        memory = Memory(); struct.pack_into('<I', memory.blocks[0x22000], 0xC, 3)
        with self.assertRaises(RuntimeError): inventory(memory, memory.base)
    def test_changes_rejected(self):
        memory = Memory(); original = memory.exact; reads = [0]
        def changed(address, size):
            if address == 0x22000:
                reads[0] += 1
                if reads[0] == 2: memory.blocks[0x22000][0x48] += 1
            return original(address, size)
        memory.exact = changed
        with self.assertRaises(RuntimeError): inventory(memory, memory.base)

if __name__ == '__main__': unittest.main()
