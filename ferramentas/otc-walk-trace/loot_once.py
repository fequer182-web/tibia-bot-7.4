"""Transfere uma quantidade de moedas 3031 de um dead rat aberto para a bag validada."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import signal
import subprocess
import sys
import time
from bag_read import BagReader, verify_build


def by_id(containers, cid):
    matches = [c for c in containers if c['container_id'] == cid]
    if len(matches) != 1:
        raise ValueError(f'Container {cid} nao esta aberto ou e ambiguo')
    return matches[0]


def coins(container):
    total = 0
    for item in container['items']:
        if item['id'] == 3031:
            amount = item['raw_count_or_subtype']
            if not 1 <= amount <= 100:
                raise ValueError('Quantidade de moedas fora do formato validado')
            total += amount
    return total


def plan(containers, source, destination, count):
    if not (0 <= source <= 63 and 0 <= destination <= 63 and source != destination and 1 <= count <= 100):
        raise ValueError('Origem/destino/quantidade invalidos')
    src, dst = by_id(containers, source), by_id(containers, destination)
    if not src['bag_item'] or src['bag_item']['id'] != 3994:
        raise ValueError('Origem precisa ser o corpo dead rat ID 3994 nesta versao')
    if not dst['bag_item'] or dst['bag_item']['id'] != 2853:
        raise ValueError('Destino precisa ser a bag ID 2853 nesta versao')
    choices = [i for i in src['items'] if i['id'] == 3031 and count <= i['raw_count_or_subtype'] <= 100]
    if not choices:
        raise ValueError('Corpo sem uma pilha de moedas 3031 com a quantidade solicitada; nenhuma chamada')
    fits_stack = any(i['id'] == 3031 and 1 <= i['raw_count_or_subtype'] <= 100-count for i in dst['items'])
    if not fits_stack and dst['item_count'] >= dst['capacity']:
        raise ValueError('Bag sem espaco para essa quantidade')
    return {'source': source, 'destination': destination, 'slot': choices[0]['slot'], 'count': count,
            'source_total_before': coins(src), 'destination_total_before': coins(dst),
            'source_object': src['address'], 'destination_object': dst['address'],
            'source_bag_object': src['bag_item']['address'], 'destination_bag_object': dst['bag_item']['address']}


def transfer_confirmed(current, request):
    src, dst = by_id(current, request['source']), by_id(current, request['destination'])
    if (src['address'], dst['address'], src['bag_item']['address'], dst['bag_item']['address']) != (
        request['source_object'], request['destination_object'], request['source_bag_object'], request['destination_bag_object']):
        raise ValueError('Identidade do corpo/bag mudou durante o teste')
    return coins(src) == request['source_total_before']-request['count'] and coins(dst) == request['destination_total_before']+request['count']


def stable(reader, memory):
    memory.alive()
    first = reader.scan()
    time.sleep(0.1)
    second = reader.scan()
    if first != second:
        raise ValueError('Containers mudaram entre leituras')
    return second


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--thread', type=int, required=True)
    parser.add_argument('--source', type=int, default=1)
    parser.add_argument('--destination', type=int, default=0)
    parser.add_argument('--count', type=int, default=1)
    args = parser.parse_args()
    print('[VERSAO] Loot Once 2')
    from value_scan import Memory, choose_pid
    folder = Path(__file__).resolve().parent
    native_exe = folder/'OTC_Loot_Once.exe'
    if not native_exe.is_file():
        raise ValueError('OTC_Loot_Once.exe precisa estar ao lado do script')
    memory = Memory(choose_pid('Antica Retro opengl.exe', args.pid))
    stop_requested = False
    def stop(signum, frame):
        nonlocal stop_requested
        stop_requested = True
        print('[PARADA] Aguardando o executavel restaurar o contexto e encerrar; nao repetira.')
    old_signal = signal.signal(signal.SIGINT, stop)
    report_path = folder/('loot_once_'+datetime.now().strftime('%Y%m%d_%H%M%S_%f')+'.json')
    report = {'utc': datetime.now(timezone.utc).isoformat(), 'pid': args.pid, 'mode': 'one_coin_transfer_request',
              'allowed_item_ids': [3031], 'status': 'preflight', 'confirmed': False}
    try:
        base, executable = verify_build(memory)
        reader = BagReader(memory, base)
        before = stable(reader, memory)
        request = plan(before, args.source, args.destination, args.count)
        report.update(before=before, request=request, executable=executable, base=hex(base),
                      version=2, empty_container_ids_before=reader.empty_entries)
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(f"[PLANO] ID 3031; quantidade={args.count}; corpo={args.source}, slot={request['slot']}; bag={args.destination}")
        if stop_requested:
            raise ValueError('Parado antes de iniciar a chamada')
        command = [str(native_exe), '--pid', str(args.pid), '--thread', str(args.thread), '--source', str(args.source),
                   '--destination', str(args.destination), '--slot', str(request['slot']), '--count', str(args.count)]
        # Nao mata o executavel por timeout/Ctrl+C enquanto uma chamada esta em andamento.
        child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        lines = []
        for line in child.stdout:
            print(line, end='', flush=True)
            lines.append(line.rstrip())
        code = child.wait()
        report.update(native_exit_code=code, native_output=lines)
        native_path = next((Path(line.removeprefix('[JSON] ')) for line in reversed(lines) if line.startswith('[JSON] ')), None)
        native = json.loads(native_path.read_text(encoding='utf-8')) if native_path and native_path.is_file() else None
        report.update(native_report_path=str(native_path) if native_path else None, native=native)
        completed = code == 0 and native and all(native.get(k) for k in ('started', 'returned', 'context_restored', 'stack_slot_restored', 'detached')) and not native.get('diagnostics')
        if not completed:
            report['status'] = 'native_call_not_confirmed'
            print('[NAO CONFIRMADO] Execucao/restauracao nao confirmada. Nao repetiu a chamada.')
            return 2
        deadline = time.monotonic()+5
        report['status'] = 'transfer_not_confirmed'
        while time.monotonic() < deadline and not stop_requested:
            try:
                after = stable(reader, memory)
                report['after'] = after
                report['empty_container_ids_after'] = reader.empty_entries
                if transfer_confirmed(after, request):
                    report.update(status='transfer_confirmed_by_container_deltas', confirmed=True)
                    print(f"[CONFIRMADO] Corpo: {request['source_total_before']} -> {request['source_total_before']-args.count}; bag: {request['destination_total_before']} -> {request['destination_total_before']+args.count} moedas.")
                    return 0
            except (ValueError, RuntimeError) as error:
                report['observation_diagnostic'] = str(error)
            time.sleep(0.2)
        print('[NAO CONFIRMADO] Quantidades nao confirmadas no prazo. Nao repetiu a chamada.')
        return 2
    except Exception as error:
        report.update(status='error', error=str(error))
        raise
    finally:
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(f'[RELATORIO] {report_path}')
        signal.signal(signal.SIGINT, old_signal)
        memory.close()


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f'[ERRO] {error}', file=sys.stderr)
        raise SystemExit(1)
