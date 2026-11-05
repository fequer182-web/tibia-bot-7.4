"""Abre um corpo de rato adjacente e recolhe suas moedas 3031, com verificacao por etapa."""
import argparse
from datetime import datetime,timezone
import json
from pathlib import Path
import signal
import sys
import time
from bag_read import verify_build
from world_read import WorldReader
from loot_once import by_id,coins,stable
from cycle_common import run_child,native_completed


def pick_corpse(world,position=None,expected=None):
    choices=world.corpses_at(position) if position else world.nearby_corpses()
    if expected:choices=[v for v in choices if int(v['object'],16)==expected]
    if len(choices)!=1:
        details='; '.join(f"{v['position']} objeto={v['object']}" for v in choices)
        raise ValueError('Precisa de um corpo de rato unico adjacente; candidatos: '+(details or 'nenhum'))
    choice=choices[0]
    player=world.player();p=choice['position']
    if p['z']!=player['z'] or max(abs(p['x']-player['x']),abs(p['y']-player['y']))>1:
        raise ValueError('Corpo fora de alcance deste teste')
    return choice


def open_and_collect(memory,world,pid,thread,destination,report,stop,position=None,expected=None,open_only=False,runner=run_child,pulse=lambda:None):
    folder=Path(__file__).resolve().parent
    before=stable(world,memory)
    bag=by_id(before,destination)
    if not bag['bag_item'] or bag['bag_item']['id']!=2853:
        raise ValueError('Destino precisa ser a bag 2853 aberta')
    corpse=pick_corpse(world,position,expected)
    time.sleep(0.1)
    if pick_corpse(world,corpse['position'],int(corpse['object'],16))!=corpse:
        raise ValueError('Corpo mudou entre as leituras')
    report.update(status='opening',before_open=before,corpse=corpse,destination=destination)
    print(f"[CORPO] ID 3994; posicao={corpse['position']}; objeto={corpse['object']}")
    pulse()
    if stop():raise ValueError('Parado antes da abertura')
    p=corpse['position']
    opened=runner([folder/'OTC_Corpse_Open_Once.exe','--pid',pid,'--thread',thread,'--x',p['x'],'--y',p['y'],'--z',p['z'],'--object',corpse['object']],'[JSON] ')
    report['native_open']=opened
    if not native_completed(opened):raise ValueError('Abertura nativa nao concluiu/restaurou; nao transfere')
    container_id=int(opened['report']['raw_rax_at_return'],16)&0xffffffff
    if not 0<=container_id<=63 or any(c['container_id']==container_id for c in before):
        raise ValueError('Open nao retornou uma janela nova disponivel; nao transfere')
    report['source_container']=container_id
    deadline=time.monotonic()+5;current=None
    while time.monotonic()<deadline and not stop():
        pulse()
        try:
            sample=stable(world,memory)
            source=by_id(sample,container_id)
            if source['bag_item'] and source['bag_item']['id']==3994:
                current=sample;break
        except (ValueError,RuntimeError):pass
        time.sleep(0.2)
    if current is None:raise ValueError('Nova janela do corpo nao apareceu; nenhuma transferencia')
    report['after_open']=current
    source=by_id(current,container_id);destination_bag=by_id(current,destination)
    source_object=source['address'];source_bag_object=source['bag_item']['address'];destination_object=destination_bag['address']
    initial_coins=coins(source);initial_bag_coins=coins(destination_bag)
    report.update(open_confirmed=True,source_coins_initial=initial_coins,destination_coins_initial=initial_bag_coins,moves=[])
    print(f'[ABERTO] Corpo na janela {container_id}, moedas={initial_coins}')
    if open_only:
        report.update(status='open_confirmed',confirmed=True);return
    collected=0
    for step in range(9):
        pulse()
        if stop():raise ValueError('Parada solicitada; nenhuma nova transferencia')
        current=stable(world,memory)
        source=by_id(current,container_id);bag=by_id(current,destination)
        if source['address']!=source_object or source['bag_item']['address']!=source_bag_object or bag['address']!=destination_object:
            raise ValueError('Identidade do corpo/bag mudou; parou')
        remaining=coins(source)
        if remaining==0:
            if collected!=initial_coins or coins(bag)!=initial_bag_coins+collected:
                raise ValueError('Totais finais divergentes; parou')
            report.update(status='all_coins_confirmed',confirmed=True,collected=collected,after=current)
            print(f'[LOOT CONCLUIDO] {collected} moedas recolhidas; nenhuma moeda 3031 restante no corpo.');return
        if remaining!=initial_coins-collected:
            raise ValueError('Quantidade do corpo mudou fora da transferencia confirmada')
        if step == 8:
            break
        pile=next(v for v in source['items'] if v['id']==3031)
        count=pile['raw_count_or_subtype']
        print(f'[LOOT {step+1}] Pedindo {count} moedas, sem repetir chamada nao confirmada.')
        move=runner([sys.executable,folder/'loot_once.py','--pid',pid,'--thread',thread,'--source',container_id,'--destination',destination,'--count',count],'[RELATORIO] ')
        report['moves'].append(move)
        if move['exit_code']!=0 or not move['report'] or not move['report'].get('confirmed'):
            raise ValueError('Transferencia nao confirmada; parou sem repetir')
        collected+=count;report['collected']=collected
    raise ValueError('Limite de oito transferencias atingido; parou')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid',type=int,required=True);parser.add_argument('--thread',type=int,required=True)
    parser.add_argument('--destination',type=int,default=0);parser.add_argument('--open-only',action='store_true')
    parser.add_argument('--x',type=int);parser.add_argument('--y',type=int);parser.add_argument('--z',type=int)
    parser.add_argument('--object',type=lambda v:int(v,0))
    args=parser.parse_args()
    specified=[v is not None for v in (args.x,args.y,args.z)]
    if any(specified) and not all(specified):raise ValueError('Informe x,y,z juntos')
    position={'x':args.x,'y':args.y,'z':args.z} if all(specified) else None
    from value_scan import Memory,choose_pid
    memory=Memory(choose_pid('Antica Retro opengl.exe',args.pid))
    stopped=False
    def handler(signum,frame):
        nonlocal stopped
        stopped=True;print('[PARADA] Aguarda a chamada atual retornar; nao inicia outra acao.')
    old=signal.signal(signal.SIGINT,handler)
    report={'mode':'open_rat_corpse_and_collect_coins','allowed_item_ids':[3031],'confirmed':False,'utc':datetime.now(timezone.utc).isoformat()}
    target=Path(__file__).resolve().parent/('open_loot_'+datetime.now().strftime('%Y%m%d_%H%M%S_%f')+'.json')
    try:
        base,path=verify_build(memory);report.update(pid=args.pid,base=hex(base),executable=path)
        open_and_collect(memory,WorldReader(memory,base),args.pid,args.thread,args.destination,report,lambda:stopped,position,args.object,args.open_only)
        return 0
    except Exception as error:
        report.update(status='stopped_on_error',error=str(error));raise
    finally:
        target.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        print(f'[RELATORIO] {target}');memory.close();signal.signal(signal.SIGINT,old)


if __name__=='__main__':
    try:raise SystemExit(main())
    except Exception as error:
        print(f'[ERRO] {error}',file=sys.stderr);raise SystemExit(1)
