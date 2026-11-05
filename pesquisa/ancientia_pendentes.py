"""Coleta conjunta dos atributos pendentes; nao altera o jogo."""
import argparse
import ctypes
from datetime import datetime,timezone
import json
import math
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
import psutil
from cromin.readers.memory import Memory
from cromin.readers.ancientia import DX
from cromin.readers.ancientia_pending import connect,snapshot,refine_field,SPECS

LABELS = {'maxMana':'Mana maxima','levelPercent':'Porcentagem do nivel (0 a 100)',
          'capacity':'Capacidade LIVRE (numero exibido)', 'magicLevel':'Magic level',
          'soul':'Soul','staminaMinutes':'Stamina em MINUTOS (ex.: 42:00 = 2520)'}


def ask(field):
    while True:
        value = input(LABELS[field]+' [Enter pula]: ').strip().replace(',','.')
        if not value:
            return None
        try:
            number = float(value)
            if math.isfinite(number) and 0 <= number <= 1e9 and (field != 'levelPercent' or number <= 100):
                return number
        except ValueError:
            pass
        print('Valor numerico invalido. Use apenas numero, sem separador de milhar.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid',required=True,type=int)
    parser.add_argument('--somente-monitor',action='store_true',help='Nao pede atributos; Enter faz nova leitura')
    args = parser.parse_args()
    if sys.platform != 'win32' or ctypes.sizeof(ctypes.c_void_p)!=8 or args.pid<=0:
        parser.error('Use Windows/Python 64 bits e um PID positivo.')
    memory = None
    result = dict(schema='ancientia-pending-v1',sha256=DX['sha256'],pid=args.pid,
                  actionsEnabled=False,liveIntegrationValidated=False,samples=[],groups=[])
    path = Path(__file__).resolve().parent/'reports'/('Ancientia_pendentes_'+datetime.now().strftime('%Y%m%d_%H%M%S_%f')+'.json')
    def save():
        path.parent.mkdir(exist_ok=True)
        path.write_text(json.dumps(result,indent=2,ensure_ascii=False),encoding='utf-8')
    try:
        memory = Memory(args.pid)
        base = connect(memory)
        result['moduleBase']=hex(base)
        result['processCreateTime']=psutil.Process(args.pid).create_time()
        print('Conectado por referencia global. Acoes desligadas. Digite sair entre coletas para encerrar.')
        print('Para separar mana maxima de porcentagem, ganhe um pouco de XP e repita sem mudar de nivel.')
        while True:
            command = input('\nEnter para coletar; sair para terminar: ').strip().casefold()
            if command in ('sair','q'):
                break
            observed = {} if args.somente_monitor else {field:ask(field) for field in SPECS}
            try:
                current,blob = snapshot(memory,base,discovery=True)
            except (OSError,RuntimeError) as error:
                result['samples'].append(dict(utc=datetime.now(timezone.utc).isoformat(),error=str(error)))
                save()
                print('[LEITURA DESCARTADA]',error)
                memory.alive()
                continue
            signature = (current['objectAddress'],tuple(current['names'].values()))
            if not result['groups'] or result['groups'][-1]['identity'] != [signature[0],list(signature[1])]:
                result['groups'].append(dict(identity=[signature[0],list(signature[1])],fields={}))
            group = result['groups'][-1]
            for field,value in observed.items():
                if value is not None:
                    group['fields'][field]=refine_field(group['fields'].get(field),blob,field,value)
            current.update(utc=datetime.now(timezone.utc).isoformat(),observed=observed,group=len(result['groups'])-1)
            result['samples'].append(current)
            save()
            k = current['known']
            print(f"{current['names']} | HP {k['health']:g}/{k['maxHealth']:g} | Mana {k['mana']:g} | XP {k['experience']:g} | Nivel {k['level']:g}")
            print('Posicoes:',current['positions'])
            print('Campos a separar:',current['maxManaCandidates'])
            for field,state in group['fields'].items():
                print(field,state['status'],state['candidateCount'],'candidatos',state['candidates'][:6])
            print('JSON:',path)
        return 0
    except (OSError,RuntimeError,ValueError,psutil.Error) as error:
        result['error']=str(error)
        print('[ERRO]',error,file=sys.stderr)
        return 1
    except (EOFError,KeyboardInterrupt):
        print('\nColeta encerrada.')
        return 130
    finally:
        if memory:
            memory.close()
        save()
        print('Relatorio:',path)


if __name__=='__main__':
    raise SystemExit(main())
