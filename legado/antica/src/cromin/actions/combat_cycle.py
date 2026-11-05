from cromin.core.paths import BIN, REPORTS, PROJECT
"""Um ciclo de ataque, corpo novo configurado e loot validado por etapa."""
import argparse
from datetime import datetime,timezone
import json
from pathlib import Path
import signal
import sys
import time
from cromin.readers.bag import verify_build
from cromin.readers.world import WorldReader,RatDataError
from cromin.actions.loot_transfer import stable,by_id
from cromin.actions.common import run_child,native_completed,SkipCycle
from cromin.actions.open_loot import open_and_collect


def fresh_corpse(world,position,old_objects):
    candidates=[c for c in world.corpses_at(position) if c['object'] not in old_objects]
    if len(candidates)>1:
        raise SkipCycle('Mais de um corpo novo na ultima posicao do rato; ciclo ignorado')
    return candidates[0] if candidates else None


def tracked_observation(world,selected):
    try:
        current=(world.monster(selected['id']) if selected.get('name','Rat')!='Rat' else world.rat(selected['id']))
    except RatDataError as error:
        d=error.details
        # Este estado foi observado no mesmo Rat apos o ataque no Antica.
        # Nao basta para confirmar morte: o ciclo ainda exige um corpo novo no ultimo tile valido.
        if (d.get('reason')=='invalid_position_or_health' and d.get('hp')==0
                and d.get('position')=={'x':65535,'y':65535,'z':255}
                and d.get('id')==selected['id'] and d.get('actual_id')==selected['id']
                and d.get('object')==selected['object']
                and ('control' not in selected or d.get('control')==selected['control'])):
            return None,dict(kind='cached_dead_off_map',details=d)
        raise
    if current is None:return None,{'kind':'creature_id_absent_or_empty'}
    if current['object']!=selected['object']:
        raise ValueError('ID do rato reutilizado; parou')
    if 'control' in selected and current.get('control')!=selected['control']:
        raise ValueError('Controle do rato mudou; parou')
    if 'name' in selected and current.get('name')!=selected['name']:
        raise SkipCycle('Nome do alvo mudou; acompanhamento ignorado')
    return current,None


def cycle(memory,world,pid,thread,destination,seconds,report,stop,runner=run_child,pulse=lambda:None,collect=lambda:True,chase=None,follow=None,selected=None,loot_policy=None):
    folder=BIN
    if collect() and (loot_policy is None or loot_policy.take):
        bag=by_id(stable(world,memory),destination)
        if not bag['bag_item'] or (loot_policy is None and bag['bag_item']['id']!=2853):
            raise ValueError('Abra a bag 2853 antes de iniciar')
    initial_player=world.player()
    report['player_at_selection']=initial_player
    try:
        candidates=[selected] if selected is not None else world.rats_on_floor()
    finally:
        report['selection_diagnostics']=getattr(world,'rat_diagnostics',[])
        report['rejected_candidates']=getattr(world,'rat_rejected_count',0)
    if report['rejected_candidates']:
        print(f"[SELECAO] {report['rejected_candidates']} candidatos rejeitados; detalhes no relatorio.")
        for item in report['selection_diagnostics'][:5]:
            print(f"[DESCARTADO] ID={item['id']} posicao={item.get('position')} vida={item.get('hp')} motivo={item['reason']}")
    report['same_floor_candidates']=candidates
    print(f"[ANDAR] jogador z={initial_player['z']}; monstros vivos com posicao valida neste andar={len(candidates)}")
    if not candidates:
        raise SkipCycle(f"Nenhum Rat vivo com posicao valida no mesmo andar do jogador (z={initial_player['z']})")
    rat=candidates[0]
    old_objects={c['object'] for c in world.nearby_corpses(initial_player)}
    surveyed={(initial_player['x']+dx,initial_player['y']+dy,initial_player['z'])
              for dx in (-1,0,1) for dy in (-1,0,1)}
    target_tile=tuple(rat['position'][key] for key in ('x','y','z'))
    if target_tile not in surveyed:
        old_objects.update(c['object'] for c in world.corpses_at(rat['position']))
        surveyed.add(target_tile)
    report.update(version=5,rat_selected=rat,old_corpse_objects=sorted(old_objects),status='attacking',tracking=[])
    if stop():raise ValueError('Parado antes do ataque')
    print(f"[ALVO] ID={rat['id']} posicao={rat['position']}. Um ataque.")
    command=[folder/'OTC_Rat_Attack_Once.exe','--pid',pid,'--thread',thread,'--id',rat['id']]
    if selected is not None:command+=['--monster-name-hex',rat.get('name','Rat').encode('utf-8').hex(),'--object',rat['object']]
    if chase is not None:command+=['--chase-mode',int(chase)]
    attack=runner(command,'[JSON] ')
    report['attack']=attack
    if not native_completed(attack,('attack_requested','attack_returned','owned_reference_handled')):
        raise ValueError('Ataque nao confirmado/restaurado; nao repete')
    data=attack['report']
    if chase is not None and (not data.get('chase_verified') or data.get('chase_mode_requested')!=int(chase)):
        raise RuntimeError('Follow nao confirmado; nao repete ataque')
    if data['rat_id']!=rat['id']:raise ValueError('ID atacado diverge do selecionado')
    if data['rat_object']!=rat['object']:raise ValueError('Objeto atacado diverge do rato selecionado')
    last_position=data['rat_position_at_request']
    deadline=time.monotonic()+seconds
    report['status']='waiting_for_new_corpse'
    body=None;exit_reads=0
    while time.monotonic()<deadline:
        pulse()
        if stop():raise ValueError('Parado; nao abre corpo nem transfere')
        player=world.player()
        if player['z']!=initial_player['z']:
            raise ValueError('Jogador mudou de andar; parou')
        try:
            current,exit_evidence=tracked_observation(world,rat)
        except RatDataError as error:
            d=error.details
            # A cached living creature may leave the loaded map. This is not death evidence.
            if (d.get('reason')=='invalid_position_or_health'
                    and isinstance(d.get('hp'),int) and 0<d['hp']<=100
                    and d.get('position')=={'x':65535,'y':65535,'z':255}
                    and d.get('id')==rat['id'] and d.get('actual_id')==rat['id']
                    and d.get('object')==rat['object']
                    and ('control' not in rat or d.get('control')==rat['control'])):
                report.update(status='target_lost',confirmed=False,collected=0,loot_skipped=True,
                              lost_target_details=d,last_valid_position=dict(last_position))
                print('[ALVO INDISPONIVEL] Rat vivo fora do mapa carregado; nao abre corpo nem coleta.')
                return
            raise
        if current:
            exit_reads=0
            last_position=current['position']
            if last_position['z']!=initial_player['z']:raise ValueError('Rato mudou de andar; parou')
            key=tuple(last_position[k] for k in ('x','y','z'))
            if current['hp']>0:
                # Registra corpos ja presentes enquanto o mesmo rato ainda esta vivo neste tile.
                old_objects.update(c['object'] for c in world.corpses_at(last_position))
                surveyed.add(key)
        if follow:follow(rat,current,player,report)
        p=last_position
        observation={'player':player,'rat':dict(p),'rat_present':current is not None}
        if not report['tracking'] or report['tracking'][-1]!=observation:
            report['tracking'].append(observation)
            print(f"[ACOMPANHANDO] jogador={player} rato={p} presente={current is not None}")
        if current is None:
            exit_reads+=1
            report.update(target_world_exit=exit_evidence,target_world_exit_reads=exit_reads)
            if exit_reads==1:print('[SAIDA DO MAPA] Confirmando em outra leitura e procurando corpo novo no ultimo tile valido.')
            if exit_reads<2:
                time.sleep(0.15);continue
            if tuple(p[k] for k in ('x','y','z')) not in surveyed:
                raise SkipCycle('Ultimo tile do rato nao foi observado antes da morte; corpo ignorado')
            body=fresh_corpse(world,last_position,old_objects)
            if body and p['z']==player['z'] and max(abs(p['x']-player['x']),abs(p['y']-player['y']))<=1:
                break
            body=None
        time.sleep(0.15)
    if not body:
        raise SkipCycle('Nao confirmou saida do Rat do mapa e corpo novo no ultimo tile valido dentro do prazo')
    report.update(corpse_association='target_left_world_and_new_corpse_at_last_valid_position',new_corpse=body,
                  old_corpse_objects=sorted(old_objects))
    print(f"[CORPO NOVO] {body['position']}; ID {body['id']}.")
    if not collect():
        report.update(status='target_cycle_finished_without_loot',confirmed=True,collected=0)
        return
    loot={};report['loot']=loot
    open_and_collect(memory,world,pid,thread,destination,loot,stop,body['position'],int(body['object'],16),runner=runner,pulse=pulse,loot_policy=loot_policy)
    report.update(status='cycle_confirmed',confirmed=loot.get('confirmed',False),collected=loot.get('collected',0))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid',type=int,required=True);parser.add_argument('--thread',type=int,required=True)
    parser.add_argument('--destination',type=int,default=0)
    parser.add_argument('--seconds',type=int,default=60)
    args=parser.parse_args()
    if not 1<=args.seconds<=120:parser.error('--seconds precisa estar entre 1 e 120')
    from cromin.readers.memory import Memory,choose_pid
    memory=Memory(choose_pid('Antica Retro opengl.exe',args.pid))
    stopped=False
    def handler(signum,frame):
        nonlocal stopped
        stopped=True;print('[PARADA] Aguarda retorno da chamada atual; nao inicia outra acao.')
    old=signal.signal(signal.SIGINT,handler)
    target=REPORTS/('rat_cycle_'+datetime.now().strftime('%Y%m%d_%H%M%S_%f')+'.json')
    report={'mode':'single_rat_attack_open_gold','version':5,'confirmed':False,'allowed_item_ids':[3031],
            'utc':datetime.now(timezone.utc).isoformat()}
    try:
        base,path=verify_build(memory);report.update(pid=args.pid,base=hex(base),executable=path)
        print('[VERSAO] Rat Cycle 5: acompanha a saida do mapa e o corpo do rato.')
        print('[TESTE UNICO] Bag aberta. Deixe o cliente perseguir; nao troque o alvo nem mexa nos containers.')
        cycle(memory,WorldReader(memory,base),args.pid,args.thread,args.destination,args.seconds,report,lambda:stopped)
        if report.get('status')=='target_lost':
            return 2
        print(f"[CONCLUIDO] Corpo aberto; {report['collected']} moedas confirmadas na bag.")
        return 0
    except Exception as error:
        report.update(status='stopped_on_error',error=str(error))
        if hasattr(error,'details'):report['read_error_details']=error.details
        raise
    finally:
        target.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        print(f'[RELATORIO] {target}');memory.close();signal.signal(signal.SIGINT,old)


if __name__=='__main__':
    try:raise SystemExit(main())
    except Exception as error:
        print(f'[ERRO] {error}',file=sys.stderr);raise SystemExit(1)
