"""Entrada separada para AncientiaDX; usa somente leitura."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'src'))
import argparse
import ctypes
from datetime import datetime, timezone
import json
import psutil
from cromin.readers.ancientia import diagnose, validate_target
from cromin.readers.memory import Memory, session


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', required=True, type=int)
    parser.add_argument('--mode', choices=['diagnostico', 'hp', 'mana'], default='diagnostico')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    if sys.platform != 'win32' or ctypes.sizeof(ctypes.c_void_p) != 8:
        parser.error('Use Windows com Python 64 bits; o cliente alvo continua x86.')
    if args.pid <= 0:
        parser.error('PID precisa ser positivo.')
    stamp = datetime.now().strftime('%Y%m%d_%H%M%S_%f')
    output = args.out or Path(__file__).resolve().parent/'reports'/f'Ancientia_{args.mode}_{stamp}.json'
    memory = None
    try:
        if output.exists():
            raise ValueError('Relatorio ja existe; escolha outro --out.')
        output.parent.mkdir(parents=True, exist_ok=True)
        if args.mode == 'diagnostico':
            memory = Memory(args.pid)
            report = diagnose(memory)
            report['createdUtc'] = datetime.now(timezone.utc).isoformat()
            output.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
            print(json.dumps(report, indent=2, ensure_ascii=False))
            print(f'\nRelatorio: {output.resolve()}')
            return 0 if report['stable'] else 2
        settings = argparse.Namespace(pid=args.pid, process='AncientiaDX.exe',
                    field=args.mode, types='double,float,uint32', scope='writable',
                    max_candidates=200000, output=str(output))
        print('Busca guiada de candidatos; nao confirma jogador ou offsets e nao habilita acoes.')
        return session(settings, validate=validate_target)
    except (OSError, ValueError, RuntimeError, psutil.Error) as error:
        print(f'[ERRO] {error}', file=sys.stderr)
        return 1
    except (KeyboardInterrupt, EOFError):
        print('\nEncerrado. Consulte o relatorio, se ja foi salvo.')
        return 130
    finally:
        if memory is not None:
            memory.close()


if __name__ == '__main__':
    raise SystemExit(main())
