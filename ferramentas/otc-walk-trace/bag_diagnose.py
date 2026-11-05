"""Diagnostico externo dos nos de containers; nenhuma chamada nativa ou escrita no jogo."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import struct
import sys
import time
from bag_read import BagReader, GAME_RVA, CONTAINER_VTABLE_RVA, ITEM_VTABLE_RVA, verify_build


def inspect_tree(reader):
    map_address = reader.base + GAME_RVA + 0x40
    output = {'map_address': hex(map_address), 'nodes': [], 'errors': []}
    def data(address, length):
        return reader.read(address, length)
    try:
        raw = data(map_address, 16)
        header, size = struct.unpack('<QQ', raw)
        output.update(map_raw=raw.hex(), header=hex(header), reported_count=size)
        sentinel = data(header, 0x38)
        left, root, right = struct.unpack_from('<QQQ', sentinel)
        output['sentinel'] = {'raw': sentinel.hex(), 'left': hex(left), 'root': hex(root), 'right': hex(right),
                              'color': sentinel[0x18], 'is_nil': sentinel[0x19]}
        pending = [(root, header)]
        seen = set()
        while pending and len(seen) < 256:
            address, expected_parent = pending.pop()
            if address == header:
                continue
            if address in seen:
                output['errors'].append(f'No repetido {address:#x}')
                continue
            seen.add(address)
            node = {'address': hex(address), 'expected_parent': hex(expected_parent)}
            output['nodes'].append(node)
            try:
                raw_node = data(address, 0x38)
                left, parent, right = struct.unpack_from('<QQQ', raw_node)
                key = struct.unpack_from('<I', raw_node, 0x20)[0]
                container, control = struct.unpack_from('<QQ', raw_node, 0x28)
                failures = []
                if raw_node[0x19]: failures.append('is_nil_nonzero')
                if parent != expected_parent: failures.append('parent_mismatch')
                if key > 255: failures.append('key_over_255')
                if not container: failures.append('null_container')
                node.update(raw=raw_node.hex(), left=hex(left), parent=hex(parent), right=hex(right),
                            color=raw_node[0x18], is_nil=raw_node[0x19], key_uint32=key,
                            key_int32=struct.unpack_from('<i', raw_node, 0x20)[0], key_low_byte=raw_node[0x20],
                            container=hex(container), control=hex(control), failed_checks=failures)
                # Continua apenas por ponteiros filhos de nos comuns. Nao segue a sentinela como dado.
                if raw_node[0x19] == 0:
                    pending.extend(((right, address), (left, address)))
                if container:
                    try:
                        raw_container = data(container, 0x90)
                        vt = struct.unpack_from('<Q', raw_container)[0]
                        cid, capacity = struct.unpack_from('<II', raw_container, 0x20)
                        item = struct.unpack_from('<Q', raw_container, 0x28)[0]
                        details = {'raw': raw_container.hex(), 'vtable': hex(vt),
                                   'expected_vtable': hex(reader.base+CONTAINER_VTABLE_RVA),
                                   'matches_vtable': vt == reader.base+CONTAINER_VTABLE_RVA,
                                   'id_uint32': cid, 'id_int32': struct.unpack_from('<i', raw_container, 0x20)[0],
                                   'capacity': capacity, 'bag_item_pointer': hex(item),
                                   'deque': [hex(v) for v in struct.unpack_from('<QQQQ', raw_container, 0x70)]}
                        node['container_data'] = details
                        if details['matches_vtable']:
                            try: details['name'] = reader.string(container+0x38)
                            except Exception as error: details['name_error'] = str(error)
                        if item:
                            try:
                                raw_item = data(item, 0x48)
                                item_vt = struct.unpack_from('<Q', raw_item)[0]
                                details['bag_item'] = {'raw': raw_item.hex(), 'vtable': hex(item_vt),
                                                       'matches_item_vtable': item_vt == reader.base+ITEM_VTABLE_RVA,
                                                       'id_at_40': struct.unpack_from('<H', raw_item, 0x40)[0]}
                            except Exception as error: details['bag_item_error'] = str(error)
                    except Exception as error: node['container_error'] = str(error)
            except Exception as error: node['read_error'] = str(error)
        output.update(visited=len(seen), traversal_truncated=bool(pending), map_unchanged=data(map_address,16)==raw,
                      sentinel_unchanged=data(header,0x38)==sentinel)
    except Exception as error:
        output['errors'].append(str(error))
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    args = parser.parse_args()
    from value_scan import Memory, choose_pid
    memory = Memory(choose_pid('Antica Retro opengl.exe', args.pid))
    try:
        base, executable = verify_build(memory)
        reader = BagReader(memory, base)
        report = {'mode':'external_read_only_container_diagnostic', 'native_call_executed':False,
                  'pid':args.pid, 'base':hex(base), 'executable':executable,
                  'utc':datetime.now(timezone.utc).isoformat(), 'samples':[]}
        for index in range(3):
            memory.alive()
            sample = inspect_tree(reader)
            report['samples'].append(sample)
            print(f"[AMOSTRA {index+1}] containers={sample.get('reported_count')} nos_lidos={sample.get('visited')}")
            for node in sample['nodes']:
                detail = node.get('container_data', {})
                print(f"  chave={node.get('key_uint32')} id={detail.get('id_uint32')} nome={detail.get('name')!r} falhas={node.get('failed_checks')} erro={node.get('read_error', '')}")
            for error in sample['errors']:print('[DIAGNOSTICO]', error)
            if index < 2:time.sleep(0.25)
        target = Path(__file__).resolve().parent/('bag_diagnostic_'+datetime.now().strftime('%Y%m%d_%H%M%S_%f')+'.json')
        target.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        print(f'[JSON] {target}')
        print('Diagnostico somente leitura concluido; nenhuma transferencia executada.')
    finally:
        memory.close()


if __name__ == '__main__':
    try:main()
    except Exception as error:
        print(f'[ERRO] {error}',file=sys.stderr)
        raise SystemExit(1)
