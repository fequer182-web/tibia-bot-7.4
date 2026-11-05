"""Coleta a lista de bolsas/corpos abertos para preparar o loot."""
import argparse
from datetime import datetime
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent / 'src'))
from cromin.readers.memory import Memory
from cromin.readers.ancientia_pending import connect, snapshot
from cromin.readers.ancientia_loot import inventory

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent / 'reports'
    root.mkdir(exist_ok=True)
    report = dict(pid=args.pid, timestamp=datetime.now().isoformat(), mode='loot_inventory', revision=3, writes=False)
    memory = None
    try:
        memory = Memory(args.pid)
        base = connect(memory)
        player, _ = snapshot(memory, base)
        report.update(player=player, moduleBase=hex(base))
        # Trace limitado ao leitor de inventario, inclusive quando ele rejeita um campo.
        trace = []
        report['inventoryReads'] = trace
        original_exact = memory.exact
        def traced(address, size):
            entry = dict(address=hex(address), size=size)
            trace.append(entry)
            del trace[:-40]
            try:
                data = original_exact(address, size)
                entry['prefixHex'] = data[:80].hex()
                return data
            except Exception as error:
                entry['error'] = str(error)
                raise
        memory.exact = traced
        report.update(containers=inventory(memory, base), status='ok')
        print('Recipientes abertos:')
        for bag in report['containers']:
            print(f"  ID {bag['containerId']}: {bag['count']}/{bag['capacity']} slots")
            for item in bag['items']:
                print(f"    slot {item['slot']}: item {item['itemId']} (subtipo bruto {item['rawSubtype']})")
        if not report['containers']:
            print('Nenhum. Abra a mochila e um corpo, e rode novamente.')
    except Exception as error:
        report.update(status='error', error=str(error))
        print('Leitura interrompida:', error)
    finally:
        if memory is not None:
            memory.close()
    path = root / ('Ancientia_loot_' + datetime.now().strftime('%Y%m%d_%H%M%S_%f') + '.json')
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print('Relatorio:', path)
    return 0 if report['status'] == 'ok' else 1

if __name__ == '__main__':
    raise SystemExit(main())
