"""Inventario x86 da build Ancientia validada; nenhuma escrita ou chamada nativa."""
import struct
from cromin.readers.ancientia_pending import decode_name
from cromin.readers.ancientia_profile import GAME_RVA

CONTAINER_VT = 0x8613B8
ITEM_VT = 0x861318

def pointer(value):
    if not 0x10000 <= value <= 0xFFFFFF00:
        raise RuntimeError(f'Ponteiro de inventario invalido: 0x{value:X}')
    return value

def u32(memory, address):
    return struct.unpack('<I', memory.exact(pointer(address), 4))[0]

def registry(memory, base):
    address = base + GAME_RVA + 0x40
    header = memory.exact(address, 8)
    head, count = struct.unpack('<II', header)
    pointer(head)
    if count > 64:
        raise RuntimeError('Quantidade de recipientes fora do limite')
    sentinel = memory.exact(head, 16)
    if sentinel[13] != 1:
        raise RuntimeError('Sentinela da lista de recipientes divergente')
    root = struct.unpack_from('<I', sentinel, 4)[0]
    pending = [root]; seen = set(); result = []
    while pending:
        node = pending.pop()
        if node == head:
            continue
        pointer(node)
        if node in seen or len(seen) >= count:
            raise RuntimeError('Arvore de recipientes inconsistente')
        seen.add(node)
        data = memory.exact(node, 24)
        if data[13] != 0:
            raise RuntimeError('No de recipiente invalido')
        left, parent, right = struct.unpack_from('<III', data)
        key, obj = struct.unpack_from('<iI', data, 16)
        if not 0 <= key < 64:
            raise RuntimeError('ID de recipiente invalido')
        # Game::getContainer aceita valores intrusive_ptr nulos na arvore.
        # A entrada continua contando como no, mas nao e recipiente aberto.
        result.append((key, pointer(obj) if obj else 0))
        pending.extend((left, right))
    if len(result) != count or len({key for key, _ in result}) != count:
        raise RuntimeError('Contagem da arvore divergente')
    if memory.exact(address, 8) != header:
        raise RuntimeError('Recipientes mudaram durante a leitura; tente novamente')
    return sorted(result)

def container(memory, base, key, obj):
    prefix = memory.exact(obj, 0x50)
    if struct.unpack_from('<I', prefix)[0] != base + CONTAINER_VT:
        raise RuntimeError('Classe de recipiente divergente')
    ident, capacity = struct.unpack_from('<II', prefix, 0xC)
    table, table_size, offset, count = struct.unpack_from('<IIII', prefix, 0x40)
    if ident != key or not 1 <= capacity <= 4096 or count > capacity:
        raise RuntimeError('Identidade/capacidade de recipiente divergente')
    if count and (not 1 <= table_size <= 65536 or table_size & (table_size - 1) or count > table_size * 4):
        raise RuntimeError('Deque de itens invalido')
    items = []
    for slot in range(count):
        index = (offset + slot) & 0xFFFFFFFF
        block = u32(memory, pointer(table) + ((index >> 2) & (table_size - 1)) * 4)
        cell = pointer(block) + (index & 3) * 4
        item = pointer(u32(memory, cell))
        raw = memory.exact(item, 0x62)
        if struct.unpack_from('<I', raw)[0] != base + ITEM_VT:
            raise RuntimeError('Classe de item divergente')
        item_id = struct.unpack_from('<H', raw, 0x3D)[0]
        if not item_id:
            raise RuntimeError('ID de item vazio')
        # +0x60 tambem armazena subtipo: nao afirmar quantidade sem ThingType.
        subtype = struct.unpack_from('<H', raw, 0x60)[0]
        if u32(memory, cell) != item or memory.exact(item, 0x62) != raw:
            raise RuntimeError('Item mudou durante a leitura; tente novamente')
        items.append(dict(slot=slot, itemId=item_id, rawSubtype=subtype, address=hex(item)))
    if memory.exact(obj, 0x50) != prefix:
        raise RuntimeError('Recipiente mudou durante a leitura; tente novamente')
    return dict(containerId=key, address=hex(obj), name=decode_name(memory,prefix,0x18), capacity=capacity, count=count, items=items)

def inventory(memory, base):
    before = registry(memory, base)
    result = [container(memory, base, key, obj) for key, obj in before if obj]
    if registry(memory, base) != before:
        raise RuntimeError('Recipientes mudaram; captura descartada')
    return result
