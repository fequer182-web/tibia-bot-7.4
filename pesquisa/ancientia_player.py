"""Busca conjunta LocalPlayer: RTTI + seed de vida + nome/posicao/atributos."""
import argparse
import ctypes
from datetime import datetime, timezone
import json
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
import psutil
from cromin.readers.memory import Memory
from cromin.readers.ancientia_player import discover


def ask_number(label, low=0, high=1000000000):
    while True:
        value = input(label+' (Enter para pular): ').strip()
        if not value:
            return None
        try:
            result = int(value)
            if low <= result <= high:
                return result
        except ValueError:
            pass
        print(f'Informe um inteiro entre {low} e {high}.')


def main():
    root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid',type=int,required=True)
    seed = parser.add_mutually_exclusive_group(required=True)
    seed.add_argument('--hp-report',type=Path)
    seed.add_argument('--sem-seed',action='store_true')
    parser.add_argument('--buscar-instancias',action='store_true',help='Se seed falhar, varrer vtables LocalPlayer')
    args = parser.parse_args()
    if sys.platform != 'win32' or ctypes.sizeof(ctypes.c_void_p) != 8 or args.pid <= 0:
        parser.error('Use Windows/Python 64 bits e PID positivo. Cliente alvo x86.')
    memory = None
    try:
        report = json.loads(args.hp_report.read_text(encoding='utf-8-sig')) if args.hp_report else None
        print('Informe valores ATUAIS. Valores anteriores nao sao preenchidos automaticamente.')
        name = input('Nome do personagem (Enter para pular): ').strip()
        values = {key:ask_number(label,1 if key in ('health','maxHealth','level') else 0) for key,label in
                  [('health','Vida atual'),('maxHealth','Vida maxima'),('mana','Mana atual'),
                   ('maxMana','Mana maxima'),('experience','XP total'),('level','Nivel')]}
        print('Posicao atual: informe X, Y e Z, ou pule com Enter.')
        xyz = [ask_number(k,1 if k != 'Z' else 0,65534 if k != 'Z' else 15) for k in ('X','Y','Z')]
        position = xyz if all(x is not None for x in xyz) else None
        memory = Memory(args.pid)
        found = discover(memory,report,values,position,name,args.buscar_instancias)
        found['process_create_time'] = psutil.Process(args.pid).create_time()
        found['createdUtc'] = datetime.now(timezone.utc).isoformat()
        root.joinpath('reports').mkdir(exist_ok=True)
        output = root/'reports'/('Ancientia_player_'+datetime.now().strftime('%Y%m%d_%H%M%S_%f')+'.json')
        output.write_text(json.dumps(found,indent=2,ensure_ascii=False),encoding='utf-8')
        for c in found['candidates'][:5]:
            print(f"LocalPlayer {c['objectAddress']} | vtable RVA {c['vtableRva']} | HP offset {c['healthSeedOffset']} | nome {bool(c['nameCandidates'])} | posicao {bool(c['positionCandidates'])}")
        print(f"Objetos candidatos: {len(found['candidates'])}; referencias globais: {len(found['directGlobalCandidates'])}")
        print('RELATORIO:',output)
        return 0 if found['candidates'] else 2
    except (OSError,RuntimeError,ValueError,KeyError,psutil.Error) as error:
        print('[ERRO]',error,file=sys.stderr)
        return 1
    except (EOFError,KeyboardInterrupt):
        print('\nEncerrado.')
        return 130
    finally:
        if memory:
            memory.close()


if __name__ == '__main__':
    raise SystemExit(main())
