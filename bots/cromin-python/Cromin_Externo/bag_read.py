"""Lista containers abertos e IDs por leitura externa da build Antica validada."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import struct
import sys
import time

GAME_RVA = 0xE52A40
ITEM_VTABLE_RVA = 0xAE9BC0
CONTAINER_VTABLE_RVA = 0xAE9230


class BagReader:
    def __init__(self, memory, base):
        self.memory, self.base = memory, base

    def read(self, address, size):
        if address < 0x10000 or address + size >= 0x800000000000 or not 0 <= size <= 4096:
            raise ValueError('Ponteiro/tamanho fora dos limites')
        return self.memory.exact(address, size)

    def u64(self, address):
        return struct.unpack('<Q', self.read(address, 8))[0]

    def string(self, address):
        raw = self.read(address, 32)
        length, capacity = struct.unpack_from('<QQ', raw, 16)
        if capacity < length or length > 256 or capacity > 4096:
            raise ValueError('Nome de container fora dos limites')
        if capacity < 16:
            if capacity != 15:
                raise ValueError('Formato de string curta diferente')
            data = raw[:length]
        else:
            data = self.read(struct.unpack_from('<Q', raw)[0], length)
        return data.decode('utf-8', errors='replace')

    def item(self, pointer):
        if not pointer:
            raise ValueError('Slot com ponteiro nulo; snapshot descartado')
        raw = self.read(pointer, 0x48)
        vt = struct.unpack_from('<Q', raw)[0]
        if vt != self.base + ITEM_VTABLE_RVA:
            raise ValueError(f'VTable de Item diferente em {pointer:#x}; nao inferiu ID')
        item_id = struct.unpack_from('<H', raw, 0x40)[0]
        if not item_id:
            raise ValueError('Item ID zero; snapshot descartado')
        return {'address': hex(pointer), 'id': item_id,
                'raw_count_or_subtype': struct.unpack_from('<H', raw, 0x44)[0]}

    def container(self, pointer, key):
        raw = self.read(pointer, 0x90)
        vt = struct.unpack_from('<Q', raw)[0]
        container_id, capacity = struct.unpack_from('<II', raw, 0x20)
        if vt != self.base + CONTAINER_VTABLE_RVA or container_id != key:
            raise ValueError('Container/VTable/ID divergente; snapshot descartado')
        bag_pointer = struct.unpack_from('<Q', raw, 0x28)[0]
        table, table_size, offset, count = struct.unpack_from('<QQQQ', raw, 0x70)
        if capacity > 10000 or count > 512 or count > capacity:
            raise ValueError('Capacidade/quantidade fora dos limites deste leitor')
        if count and (not table or not table_size or table_size & (table_size-1) or table_size > 65536):
            raise ValueError('Mapa da deque invalido')
        if count and count > table_size:
            raise ValueError('Quantidade maior que mapa da deque')
        result = {'container_id': key, 'address': hex(pointer), 'name': self.string(pointer+0x38),
                  'capacity': capacity, 'item_count': count, 'bag_item': self.item(bag_pointer) if bag_pointer else None,
                  'items': []}
        # Container::getItem atual: um shared_ptr por bloco (objeto + controle).
        for index in range(count):
            slot = (offset + index) & (table_size-1)
            block = self.u64(table + slot*8)
            item_pointer = self.u64(block)
            entry = self.item(item_pointer)
            if self.u64(block) != item_pointer:
                raise ValueError('Slot mudou durante leitura')
            entry['slot'] = index
            result['items'].append(entry)
        after = self.read(pointer, 0x90)
        if after != raw:
            raise ValueError('Container mudou durante leitura; tente novamente parado')
        return result

    def scan(self):
        self.empty_entries = []
        map_address = self.base + GAME_RVA + 0x40
        meta = self.read(map_address, 16)
        header, size = struct.unpack('<QQ', meta)
        if size > 256:
            raise ValueError('Mapa com mais de 256 containers; formato divergente')
        sentinel = self.read(header, 0x20)
        if sentinel[0x19] != 1:
            raise ValueError('Cabecalho da arvore nao e sentinela MSVC esperada')
        root = struct.unpack_from('<Q', sentinel, 8)[0]
        if not size:
            if root != header:
                raise ValueError('Arvore vazia com raiz divergente')
            nodes = []
        else:
            nodes, pending, seen = [], [(root, header)], set()
            node_snapshots = []
            while pending:
                node, parent = pending.pop()
                if node == header:
                    continue
                if node in seen or len(seen) >= size:
                    raise ValueError('Ciclo ou tamanho divergente na arvore')
                seen.add(node)
                data = self.read(node, 0x38)
                left, actual_parent, right = struct.unpack_from('<QQQ', data)
                key = struct.unpack_from('<I', data, 0x20)[0]
                container = struct.unpack_from('<Q', data, 0x28)[0]
                control = struct.unpack_from('<Q', data, 0x30)[0]
                if data[0x19] or actual_parent != parent or key > 255 or (not container and control):
                    raise ValueError(f'No de container invalido: chave={key}, nil={data[0x19]}, parent={actual_parent:#x}, esperado={parent:#x}, objeto={container:#x}, controle={control:#x}')
                if not container:
                    self.empty_entries.append(key)
                nodes.append((key, container))
                node_snapshots.append((node, data))
                pending.extend(((right, node), (left, node)))
            if len(nodes) != size or len({key for key, _ in nodes}) != size:
                raise ValueError('Quantidade/IDs de containers divergentes')
        # Entradas shared_ptr {0,0} fazem parte da arvore, mas nao sao containers abertos.
        result = [self.container(pointer, key) for key, pointer in sorted(nodes) if pointer]
        if size and any(self.read(node, 0x38) != data for node, data in node_snapshots):
            raise ValueError('No de container mudou durante leitura')
        if self.read(map_address, 16) != meta or self.read(header, 0x20) != sentinel:
            raise ValueError('Mapa de containers mudou durante leitura')
        return result


def verify_build(memory):
    from build_probe_memory import parse_pe, fnv1a64
    path = memory.executable()
    if Path(path).name.lower() != 'antica retro opengl.exe':
        raise ValueError('Processo diferente do Antica esperado')
    base, size = memory.main_module(path)
    def module_read(rva, length):
        if rva < 0 or rva+length > size:
            raise ValueError('Leitura fora do modulo')
        return memory.exact(base+rva, length)
    pe = parse_pe(module_read)
    if (pe['machine'], pe['timestamp'], pe['size_image']) != (0x8664, 0x6AD91836, 0xEDE000):
        raise ValueError('Build diferente; nao aplicou offsets antigos')
    section = next(s for s in pe['sections'] if s['name'] == '.text')
    length = max(section['virtual'], section['raw'])
    if section['rva'] != 0x1000 or length != 0x95B400:
        raise ValueError('Layout de codigo diferente')
    if int(fnv1a64(module_read(section['rva'], length)), 16) != 0xBF68517610291E3A:
        raise ValueError('Hash da build diferente')
    return base, path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int)
    args = parser.parse_args()
    if sys.platform != 'win32' or struct.calcsize('P') != 8:
        raise ValueError('Use Python 64 bits no Windows')
    from value_scan import Memory, choose_pid
    memory = Memory(choose_pid('Antica Retro opengl.exe', args.pid))
    try:
        base, path = verify_build(memory)
        reader = BagReader(memory, base)
        before = reader.scan()
        time.sleep(0.15)
        memory.alive()
        after = reader.scan()
        if before != after:
            raise ValueError('As duas leituras divergiram; pare de alterar a bag e tente novamente')
        report = {'mode': 'external_read_only_bags', 'status': 'consistent_snapshot_visible_validation_pending',
                  'pid': memory.pid, 'executable': path, 'base': hex(base),
                  'utc': datetime.now(timezone.utc).isoformat(), 'timestamp': '0x6AD91836',
                  'text_hash_fnv1a64': '0xBF68517610291E3A', 'containers': after,
                  'empty_container_ids': reader.empty_entries,
                  'note': 'raw_count_or_subtype e bruto; nao e quantidade confirmada para todos os itens'}
        target = Path(__file__).resolve().parent / ('bag_read_'+datetime.now().strftime('%Y%m%d_%H%M%S_%f')+'.json')
        target.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        if not after:
            print('[BAG] Nenhum container aberto. Abra a bag no jogo e execute novamente.')
        for bag in after:
            bag_id = bag['bag_item']['id'] if bag['bag_item'] else 'indisponivel'
            print(f"[BAG] janela={bag['container_id']} nome={bag['name']!r} ID do item bag={bag_id} itens={bag['item_count']}/{bag['capacity']}")
            for item in bag['items']:
                print(f"  slot={item['slot']:2} ID={item['id']:5} count/subtype bruto={item['raw_count_or_subtype']}")
        print(f'[JSON] {target}')
        return 0
    finally:
        memory.close()


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f'[ERRO] {error}', file=sys.stderr)
        raise SystemExit(1)
