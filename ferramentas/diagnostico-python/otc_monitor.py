"""Monitor OTC externo: capacidades multiplas, leitura de estado e teste manual Attack/Follow."""
import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import struct
import sys
import time
import psutil

from build_probe_memory import parse_pe, fnv1a64
from value_scan import Memory, choose_pid

TYPES = {'float64': '<d', 'float32': '<f', 'uint32': '<I', 'uint64': '<Q', 'byte': '<B'}


def number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


class Monitor:
    def __init__(self, memory, profile):
        self.memory, self.profile = memory, profile
        self.path = memory.executable()
        self.base, self.size = memory.main_module(self.path)
        if profile.get('mode') != 'external_read_only' or profile.get('schema') != 'otclab-external-v27-port-v1':
            raise ValueError('Perfil externo desconhecido')
        pe = parse_pe(self.module_read)
        self.pe = pe
        build = profile['build']
        for field, key in [('timestamp', 'timestamp'), ('size_image', 'sizeOfImage')]:
            if pe[field] != number(build[key]):
                raise ValueError('Fingerprint PE diferente do perfil. Gere outro perfil para esta build.')
        if pe['machine'] != 0x8664:
            raise ValueError('Este monitor exige cliente x64')
        text = next(s for s in pe['sections'] if s['name'] == '.text')
        text_size = min(max(text['virtual'], text['raw']), pe['size_image'] - text['rva'], self.size - text['rva'])
        if text['rva'] != number(build['textRva']) or text_size != number(build['textSize']):
            raise ValueError('Layout da .text diferente do perfil')
        if not 0 < text_size <= 256 * 1024 * 1024:
            raise ValueError('.text fora dos limites')
        if number(fnv1a64(self.module_read(text['rva'], text_size))) != number(build['textHashFNV1A64']):
            raise ValueError('Hash .text diferente do perfil. Monitor cancelado.')
        self.text_range = (self.base + text['rva'], self.base + text['rva'] + text_size)
        self.resolve_player()

    def module_read(self, rva, size):
        if rva < 0 or size < 0 or rva + size > self.size:
            raise ValueError('Leitura fora do modulo principal')
        return self.memory.exact(self.base + rva, size)

    def pointer(self, address):
        return struct.unpack('<Q', self.memory.exact(address, 8))[0]

    def global_pointer(self, rva):
        return struct.unpack('<Q', self.module_read(rva, 8))[0]

    def resolve_player(self):
        lp = self.profile['localPlayer']
        paths = []
        direct = number(lp.get('directGlobalRva', '0x0'))
        indirect = number(lp.get('indirectGlobalRva', '0x0'))
        if direct:
            paths.append(self.global_pointer(direct))
        if indirect:
            owner = self.global_pointer(indirect)
            offset = number(lp['indirectOwnerOffset'])
            if not 0 <= offset <= 0x10000 or not owner:
                raise ValueError('Owner/offset indireto invalido')
            paths.append(self.pointer(owner + offset))
        if not paths or not paths[0] or any(p != paths[0] for p in paths):
            raise ValueError('Caminhos do jogador indisponiveis ou divergentes. Reentre no personagem ou refaca a descoberta.')
        player = paths[0]
        vtable = number(lp['vtableRva'])
        if not 0 < vtable < self.size or self.pointer(player) != self.base + vtable:
            raise ValueError('VTable do jogador nao corresponde ao perfil')
        return player

    def field(self, player, offset, kind, scale=1):
        offset = number(offset)
        if not 0 <= offset <= 0x1200 - struct.calcsize(TYPES[kind]):
            raise ValueError('Offset de atributo fora dos limites')
        value = struct.unpack(TYPES[kind], self.memory.exact(player + offset, struct.calcsize(TYPES[kind])))[0] * scale
        if not math.isfinite(value):
            raise ValueError('Atributo nao finito')
        return value

    def owner(self, player):
        game = self.profile.get('game', {})
        rva = number(game.get('globalRva', '0x0'))
        if game.get('status') != 'external_owner_resolved' or not rva:
            return None
        source = game.get('source')
        if source == 'module_direct_field':
            if not 0 < rva < self.size:
                raise ValueError('RVA do owner fora do modulo')
            owner = self.base + rva
        elif source == 'module_pointer_to_owner':
            owner = self.global_pointer(rva)
        else:
            raise ValueError('Origem do owner desconhecida')
        offset = number(game.get('localPlayerOffset', '0x0'))
        if not owner or not 0 <= offset <= 0x10000 or self.pointer(owner + offset) != player:
            raise ValueError('Owner nao aponta para o jogador atual')
        return owner

    def target(self, owner, key):
        offset = number(self.profile.get('game', {}).get(key + 'TargetOffset', '0x0'))
        if owner is None or not offset:
            return {'status': 'pending', 'pointer': None}
        if not 0 < offset <= 0x10000:
            raise ValueError('Offset de alvo fora dos limites')
        pointer = self.pointer(owner + offset)
        result = {'status': 'read', 'pointer': hex(pointer), 'plausible': False}
        if pointer:
            try:
                vt = self.pointer(pointer)
                result['vtable'] = hex(vt)
                if self.base <= vt <= self.base + self.size - 8:
                    fn = self.pointer(vt)
                    result['plausible'] = self.text_range[0] <= fn < self.text_range[1]
            except RuntimeError as error:
                result['diagnostic'] = str(error)
        return result

    def snapshot(self):
        self.memory.alive()
        player = self.resolve_player()
        result = {'utc': datetime.now(timezone.utc).isoformat(), 'player': hex(player), 'fields': {}, 'targets': {}}
        # Preserva TODOS os candidatos de cada campo, inclusive os dois de capacidade.
        for name, evidence in self.profile.get('guidedFields', {}).items():
            readings = []
            for candidate in evidence.get('candidates', []):
                kind = candidate['type']
                if kind not in TYPES:
                    raise ValueError('Tipo de atributo desconhecido')
                scale = float(candidate.get('displayScale', 1))
                if not math.isfinite(scale) or scale <= 0:
                    raise ValueError('Escala de atributo invalida')
                readings.append({'offset': candidate['offset'], 'type': kind, 'scale': scale,
                                 'value': self.field(player, candidate['offset'], kind, scale)})
            result['fields'][name] = {'evidence': evidence.get('status', 'pending'), 'readings': readings}
        result['position'] = {}
        for name in ('x', 'y', 'z', 'direction'):
            offset = number(self.profile['localPlayer'].get('position', {}).get(name, '0x0'))
            if offset:
                result['position'][name] = self.field(player, offset, 'uint32' if name in ('x', 'y') else 'byte')
        owner = self.owner(player)
        for key in ('attack', 'follow'):
            result['targets'][key] = self.target(owner, key)
        if self.resolve_player() != player:
            raise RuntimeError('Jogador mudou durante a amostra; amostra descartada')
        return result

    def static_functions(self):
        result = {}
        for name, item in self.profile.get('functions', {}).items():
            rva = number(item.get('rva', '0x0'))
            if not rva:
                continue
            address = self.base + rva
            if not self.text_range[0] <= address < self.text_range[1]:
                result[name] = {'status': 'outside_text', 'rva': hex(rva)}
                continue
            amount = min(16, self.text_range[1] - address)
            result[name] = {'status': 'static_candidate_only', 'rva': hex(rva),
                            'bytes': self.module_read(rva, amount).hex(' '), 'abi_validated': False}
        return result


def show(sample):
    print(f'\nJogador: {sample["player"]} | Posicao: {sample["position"]}', flush=True)
    for name, item in sample['fields'].items():
        for index, reading in enumerate(item['readings'], 1):
            label = f'Capacidade {index}' if name == 'capacity' else name
            print(f'  {label} (+{reading["offset"]}): {reading["value"]:g} [{item["evidence"]}]', flush=True)
    print(f'  Ataque: {sample["targets"]["attack"]["pointer"]} | Seguir: {sample["targets"]["follow"]["pointer"]}', flush=True)


def evaluate(samples, key):
    values = [s['targets'][key] for s in samples]
    ok = (len(values) == 3 and values[0]['pointer'] == '0x0'
          and values[1]['pointer'] not in (None, '0x0') and values[1].get('plausible')
          and values[2]['pointer'] == '0x0')
    return 'observed_target_transition' if ok else 'inconclusive'


def guided_test(monitor, key):
    label = 'ATAQUE' if key == 'attack' else 'SEGUIR'
    instructions = ['Cancele ataque e seguir no jogo.',
                    'Ataque uma criatura pelo jogo e mantenha o alvo.' if key == 'attack' else 'Use Seguir em uma criatura pelo jogo, sem atacar.',
                    'Cancele a acao no jogo e fique sem alvo.']
    samples = []
    print(f'\nTESTE MANUAL: {label}. Digite pular para encerrar este teste.', flush=True)
    for instruction in instructions:
        if input(instruction + ' ENTER quando pronto: ').strip().lower() in ('pular', 'sair', 'n'):
            return {'status': 'skipped', 'samples': samples}
        sample = monitor.snapshot()
        if not samples and sample['targets'][key]['pointer'] is None:
            return {'status': 'pending_offset', 'samples': [sample]}
        samples.append(sample)
        show(sample)
    return {'status': evaluate(samples, key), 'samples': samples,
            'note': 'Observa o slot de alvo; nao chama nem valida a ABI da funcao interna.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', required=True)
    parser.add_argument('--pid', type=int)
    parser.add_argument('--process', default='Antica Retro opengl.exe')
    parser.add_argument('--test', choices=['monitor', 'attack', 'follow', 'both'], default='monitor')
    parser.add_argument('--count', type=int, default=20)
    parser.add_argument('--interval', type=float, default=1)
    parser.add_argument('--output', default='otc_test_' + datetime.now().strftime('%Y%m%d_%H%M%S') + '.json')
    args = parser.parse_args()
    if not 1 <= args.count <= 3600 or not 0.1 <= args.interval <= 60:
        parser.error('count: 1..3600; interval: 0.1..60')
    memory, report = None, None
    output = Path(args.output)
    try:
        if sys.platform != 'win32' or struct.calcsize('P') != 8:
            raise ValueError('Use Windows e Python 64 bits')
        if output.exists():
            raise ValueError('Relatorio ja existe; escolha outro --output')
        # Alguns exports C++ antigos usam ANSI no caminho; o caminho registrado nao e usado para abrir o alvo.
        profile = json.loads(Path(args.profile).read_text(encoding='utf-8-sig', errors='replace'))
        memory = Memory(choose_pid(args.process, args.pid))
        monitor = Monitor(memory, profile)
        report = {'mode': 'external_read_only', 'pid': memory.pid, 'executable': monitor.path,
                  'build': profile['build'], 'functions': monitor.static_functions(), 'tests': {}, 'samples': []}
        def save():
            output.write_text(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False), encoding='utf-8')
        print(f'Build/hash e VTable conferidos. PID {memory.pid}.\nCapacidades multiplas serao mantidas separadas.', flush=True)
        print('Funcoes: ' + ', '.join(report['functions']) + ' (candidatos estaticos; nao executados).', flush=True)
        if args.test == 'monitor':
            for i in range(args.count):
                sample = monitor.snapshot()
                report['samples'].append(sample)
                show(sample)
                save()
                if i + 1 < args.count:
                    time.sleep(args.interval)
        else:
            for key in (('attack', 'follow') if args.test == 'both' else (args.test,)):
                report['tests'][key] = guided_test(monitor, key)
                print(f'[RESULTADO] {key}: {report["tests"][key]["status"]}', flush=True)
                save()
        print(f'[JSON] {output.resolve()}')
        return 0
    except (OSError, RuntimeError, ValueError, KeyError, TypeError, StopIteration, psutil.Error) as error:
        print(f'[ERRO] {error}', file=sys.stderr)
        return 1
    except (KeyboardInterrupt, EOFError):
        print('\nEncerrado; as amostras ja concluidas estao no ultimo JSON salvo.')
        return 130
    finally:
        if memory:
            memory.close()


if __name__ == '__main__':
    raise SystemExit(main())
