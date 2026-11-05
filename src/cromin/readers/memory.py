"""Cromin_py: busca externa de valores e refinamento guiado, somente leitura."""
import argparse
import ctypes as C
from ctypes import wintypes as W
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation
import json
import math
import os
from pathlib import Path
import struct
import sys
import time

import psutil
from cromin.readers.build import Reader, parse_pe

FORMATS = {'double': '<d', 'float': '<f', 'uint32': '<I', 'uint64': '<Q'}
READABLE = {0x02, 0x04, 0x08, 0x20, 0x40, 0x80}
WRITABLE = {0x04, 0x08, 0x40, 0x80}


class MBI(C.Structure):
    _fields_ = [('BaseAddress', C.c_void_p), ('AllocationBase', C.c_void_p),
                ('AllocationProtect', W.DWORD), ('PartitionId', W.WORD),
                ('RegionSize', C.c_size_t), ('State', W.DWORD),
                ('Protect', W.DWORD), ('Type', W.DWORD)]


def needles(value, types):
    try:
        number = Decimal(value.replace(',', '.'))
    except InvalidOperation:
        raise ValueError('Informe somente o numero, sem separadores de milhar')
    if not number.is_finite() or number < 0:
        raise ValueError('Use um valor numerico finito e nao negativo')
    result = {}
    for kind in types:
        try:
            if kind.startswith('uint'):
                if number != number.to_integral_value():
                    continue
                converted = int(number)
            else:
                converted = float(number)
                if not math.isfinite(converted):
                    continue
            packed = struct.pack(FORMATS[kind], converted)
            if kind in ('float', 'double') and not math.isfinite(struct.unpack(FORMATS[kind], packed)[0]):
                continue
            result[kind] = packed
        except (OverflowError, struct.error):
            continue
    if not result:
        raise ValueError('Valor nao representavel nos tipos selecionados')
    return result


class Memory(Reader):
    def __init__(self, pid):
        super().__init__(pid)
        self.api.VirtualQueryEx.argtypes = [W.HANDLE, C.c_void_p, C.POINTER(MBI), C.c_size_t]
        self.api.VirtualQueryEx.restype = C.c_size_t
        self.api.GetExitCodeProcess.argtypes = [W.HANDLE, C.POINTER(W.DWORD)]
        self.api.GetExitCodeProcess.restype = W.BOOL

    def alive(self):
        code = W.DWORD()
        if not self.api.GetExitCodeProcess(self.handle, C.byref(code)):
            raise C.WinError(C.get_last_error())
        if code.value != 259:
            raise RuntimeError('O processo encerrou. Inicie uma nova sessao.')

    def regions(self, scope):
        address = 0
        while True:
            info = MBI()
            got = self.api.VirtualQueryEx(self.handle, address, C.byref(info), C.sizeof(info))
            if not got:
                error = C.get_last_error()
                self.alive()
                if error == 87:  # Acima do maior endereco acessivel do processo.
                    return
                raise C.WinError(error)
            if got != C.sizeof(info):
                raise RuntimeError('VirtualQueryEx retornou estrutura incompleta')
            base, size = info.BaseAddress or 0, info.RegionSize
            if not size or base + size <= address:
                raise RuntimeError('Mapa de memoria invalido')
            protection = info.Protect & 0xFF
            readable = (info.State == 0x1000 and not info.Protect & 0x100
                        and protection in READABLE)
            selected = readable and (scope == 'readable' or protection in WRITABLE)
            if selected:
                yield base, size
            address = base + size

    def exact(self, address, size):
        # Reconfere protecao antes de ler: nunca tenta ler uma pagina guard conhecida.
        cursor = address
        while cursor < address + size:
            info = MBI()
            got = self.api.VirtualQueryEx(self.handle, cursor, C.byref(info), C.sizeof(info))
            if got != C.sizeof(info) or info.State != 0x1000 or info.Protect & 0x100 or (info.Protect & 255) not in READABLE:
                raise RuntimeError(f'Regiao deixou de ser legivel em 0x{cursor:X}')
            end = (info.BaseAddress or 0) + info.RegionSize
            if end <= cursor:
                raise RuntimeError('Regiao invalida')
            cursor = end
        buffer = C.create_string_buffer(size)
        count = C.c_size_t()
        ok = self.api.ReadProcessMemory(self.handle, address, buffer, size, C.byref(count))
        if not ok or count.value != size:
            raise RuntimeError(f'Leitura incompleta em 0x{address:X}; erro {C.get_last_error()}')
        return buffer.raw


def blocks(memory, base, size, stats, block_size=65536):
    end = base + size
    while base < end:
        length = min(block_size, end - base)
        try:
            data = memory.exact(base, length)
            stats['read_bytes'] += length
            yield base, data
        except RuntimeError:
            # Recupera paginas boas em vez de descartar o bloco todo.
            cursor = base
            while cursor < base + length:
                amount = min(4096 - cursor % 4096, base + length - cursor)
                try:
                    data = memory.exact(cursor, amount)
                    stats['read_bytes'] += amount
                    yield cursor, data
                except RuntimeError:
                    stats['failed_bytes'] += amount
                    stats['failed_pages'] += 1
                    if len(stats['failure_examples']) < 10:
                        stats['failure_examples'].append(hex(cursor))
                    yield cursor, None
                cursor += amount
        base += length


def find_matches(data, base, patterns, tail_size=0):
    for kind, pattern in patterns.items():
        start = 0
        while True:
            offset = data.find(pattern, start)
            if offset < 0:
                break
            # Sobreposicao de 7 bytes preserva valores atravessando blocos.
            if offset + len(pattern) > tail_size:
                yield base + offset, kind
            start = offset + 1


def initial_scan(memory, patterns, scope, limit, stats):
    hits = []
    tail, previous_end = b'', None
    last_print = time.monotonic()
    for base, size in memory.regions(scope):
        stats['selected_bytes'] += size
        stats['regions'] += 1
        for address, data in blocks(memory, base, size, stats):
            if data is None:
                tail, previous_end = b'', None
                continue
            if address != previous_end:
                tail = b''
            combined = tail + data
            for hit in find_matches(combined, address - len(tail), patterns, len(tail)):
                if len(hits) >= limit:
                    stats['complete'] = False
                    stats['stopped_reason'] = 'candidate_limit'
                    raise RuntimeError(f'Limite de {limit} candidatos atingido. Busca INCOMPLETA. '
                                       'Use valor menos comum ou selecione --types double,float. '
                                       'Nenhuma lista truncada sera refinada.')
                hits.append(hit)
            tail, previous_end = combined[-7:], address + len(data)
            if time.monotonic() - last_print >= 3:
                print(f'[SCAN] {stats["read_bytes"] / 1048576:.1f} MiB lidos; {len(hits)} candidatos', flush=True)
                memory.alive()
                last_print = time.monotonic()
    memory.alive()
    stats['complete'] = stats['failed_bytes'] == 0
    stats['stopped_reason'] = 'finished' if stats['complete'] else 'unreadable_pages'
    return hits


def refine(memory, hits, patterns):
    survivors, unreadable = [], 0
    cache = {}
    for address, kind in hits:
        if kind not in patterns:
            continue
        pattern = patterns[kind]
        page = address & ~4095
        try:
            if address + len(pattern) <= page + 4096:
                if page not in cache:
                    try:
                        cache[page] = memory.exact(page, 4096)
                    except RuntimeError:
                        cache[page] = None
                data = cache[page]
                current = (data[address - page:address - page + len(pattern)] if data is not None
                           else memory.exact(address, len(pattern)))
            else:
                current = memory.exact(address, len(pattern))
            if current == pattern:
                survivors.append((address, kind))
        except RuntimeError:
            unreadable += 1
    memory.alive()
    return survivors, unreadable


def show(hits, base, image_size):
    print(f'[CANDIDATOS] {len(hits)} combinacoes endereco/tipo; {len(set(a for a, _ in hits))} enderecos')
    for address, kind in hits[:30]:
        location = f'RVA=0x{address - base:X}' if base <= address < base + image_size else 'fora do modulo principal'
        print(f'  0x{address:X}  {kind:6}  {location}')
    if len(hits) > 30:
        print('  Exibindo 30; todos os candidatos estao no JSON.')
    print('Status: candidatos de valor; objeto LocalPlayer e offsets ainda nao confirmados.')


def choose_pid(name, pid):
    if pid:
        return pid
    matches = [p.info['pid'] for p in psutil.process_iter(['pid', 'name'])
               if (p.info['name'] or '').casefold() == name.casefold()]
    if len(matches) != 1:
        raise RuntimeError(f'Processos encontrados: {matches}. Abra o cliente ou use --pid NUMERO.')
    return matches[0]


def ask_value(prompt, types):
    while True:
        value = input(prompt).strip()
        if value.lower() in ('sair', 'q'):
            return None
        try:
            return value, needles(value, types)
        except ValueError as error:
            print(error)


def session(args, validate=None):
    types = args.types.split(',')
    if not types or any(t not in FORMATS for t in types):
        raise ValueError('Tipos validos: double,float,uint32,uint64 (separados por virgula)')
    memory = Memory(choose_pid(args.process, args.pid))
    try:
        path = memory.executable()
        base, size = memory.main_module(path)
        def read_header(offset, amount):
            if offset < 0 or offset + amount > size:
                raise ValueError('Cabecalho fora do modulo')
            return memory.exact(base + offset, amount)
        pe = parse_pe(read_header)
        target_validation = validate(memory, path, base, size, pe) if validate else None
        output = Path(args.output)
        if output.exists():
            raise ValueError(f'{output} ja existe. Escolha outro nome com --output para preservar a sessao anterior.')
        report = dict(schema=1, status='value_candidates_only', pid=memory.pid, executable=path,
                      created_utc=datetime.now(timezone.utc).isoformat(),
                      process_create_time=psutil.Process(memory.pid).create_time(),
                      module_base=hex(base), build={'timestamp': hex(pe['timestamp']),
                      'sizeOfImage': hex(pe['size_image']), 'machine': hex(pe['machine'])},
                      field=args.field, scope=args.scope, comparison='exact_encoded_bytes', samples=[])
        if target_validation is not None:
            report['targetValidation'] = target_validation
        print(f'PID {memory.pid} | {path}\nCampo: {args.field} | Tipos: {", ".join(types)}')
        print('Informe o valor atual. Evite 0 e valores pequenos/muito comuns. Use sair para encerrar.')
        print('Busca exata: valores arredondados na interface podem nao coincidir com a memoria.')
        entry = ask_value('Valor inicial: ', types)
        if entry is None:
            return 0
        value, patterns = entry
        stats = dict(selected_bytes=0, read_bytes=0, failed_bytes=0, failed_pages=0,
                     regions=0, failure_examples=[], complete=False, stopped_reason='running')
        try:
            hits = initial_scan(memory, patterns, args.scope, args.max_candidates, stats)
        except (RuntimeError, KeyboardInterrupt):
            report['coverage'] = stats
            report['status'] = 'initial_scan_incomplete'
            output.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
            raise
        report['coverage'] = stats
        def save(current, entered, unreadable=0):
            report['samples'].append(dict(value=entered, utc=datetime.now(timezone.utc).isoformat(),
                                          candidate_count=len(current), unreadable_candidates=unreadable))
            report['candidates'] = [dict(address=hex(a), type=t,
                module_rva=hex(a-base) if base <= a < base+size else None) for a, t in current]
            output.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
            show(current, base, size)
            print(f'[JSON] {output.resolve()}')
        print(f'[COBERTURA] {stats["read_bytes"]} bytes lidos; {stats["failed_bytes"]} bytes inacessiveis; '
              f'{stats["regions"]} regioes selecionadas. Escopo: {args.scope}.')
        save(hits, value)
        if not stats['complete']:
            print('[AVISO] Cobertura incompleta; regioes inacessiveis estao registradas no JSON.')
        while hits:
            print('\nMude o valor no jogo, aguarde a interface estabilizar e informe o novo valor.')
            entry = ask_value('Novo valor (ou sair): ', types)
            if entry is None:
                break
            new_value, new_patterns = entry
            if new_patterns == patterns:
                print('O valor e igual ao anterior; use uma mudanca real para refinar.')
                continue
            hits, unreadable = refine(memory, hits, new_patterns)
            patterns = new_patterns
            save(hits, new_value, unreadable)
        if not hits:
            print('Nenhum candidato restante. Repita com valor estavel ou outro tipo/escopo. '
                  'O objeto pode ter mudado de endereco; esta sessao nao redescobre objetos.')
        return 0
    finally:
        memory.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--process', default='Antica Retro opengl.exe')
    parser.add_argument('--pid', type=int)
    parser.add_argument('--field', choices=['hp', 'mana'], default='hp')
    parser.add_argument('--types', default='double,float,uint32,uint64')
    parser.add_argument('--scope', choices=['writable', 'readable'], default='writable')
    parser.add_argument('--max-candidates', type=int, default=200000)
    parser.add_argument('--output', default='value_scan_' + datetime.now().strftime('%Y%m%d_%H%M%S') + '.json')
    args = parser.parse_args()
    if sys.platform != 'win32' or C.sizeof(C.c_void_p) != 8:
        parser.error('Use Windows e Python 64 bits')
    if args.max_candidates <= 0 or args.pid is not None and args.pid <= 0:
        parser.error('Limites e PID devem ser positivos')
    try:
        return session(args)
    except (OSError, ValueError, RuntimeError, psutil.Error) as error:
        print(f'[ERRO] {error}', file=sys.stderr)
        return 1
    except (KeyboardInterrupt, EOFError):
        print('\nEncerrado; consulte o ultimo JSON salvo, se houver.')
        return 130


if __name__ == '__main__':
    raise SystemExit(main())
