"""One native attack on a validated Monster, then observe that same identity."""
import time
from cromin.core.paths import BIN
from cromin.actions.common import run_child,native_completed,SkipCycle
from cromin.readers.world import RatDataError

def attack_target(memory,world,pid,thread,selected,report,stop,runner=run_child,pulse=lambda:None,seconds=60,clock=time.monotonic,sleep=time.sleep,chase=None,follow=None):
    report.update(status='target_preflight',selected=dict(selected),confirmed=False,loot_supported=False)
    current=world.monster(selected['id'])
    if current is None or current['hp']<=0:raise SkipCycle('Alvo desapareceu antes do ataque')
    if any(current.get(k)!=selected.get(k) for k in ('object','control','name')):raise SkipCycle('Alvo mudou antes do ataque')
    if stop():raise SkipCycle('Target desativado antes do ataque')
    command=[BIN/'OTC_Rat_Attack_Once.exe','--pid',pid,'--thread',thread,'--id',selected['id'],
        '--monster-name-hex',selected['name'].encode('utf-8').hex(),'--object',selected['object']]
    if chase is not None:command+=['--chase-mode',int(chase)]
    result=runner(command,'[JSON] ')
    report['attack']=result
    if not native_completed(result,('attack_requested','attack_returned','owned_reference_handled')):
        raise RuntimeError('Ataque/restauracao nao confirmados; nao repete')
    data=result['report']
    if chase is not None and (not data.get('chase_verified') or data.get('chase_mode_requested')!=int(chase)):
        raise RuntimeError('Follow nao confirmado; nao repete ataque')
    if data.get('rat_id')!=selected['id'] or data.get('rat_object')!=selected['object']:
        raise RuntimeError('Ataque retornou identidade diferente do alvo escolhido')
    report.update(status='tracking_target',confirmed=True)
    deadline=clock()+seconds;missing=0
    while clock()<deadline:
        pulse()
        if stop():report['status']='target_tracking_stopped';return
        player=world.player()
        if player['z']!=selected['position']['z']:raise SkipCycle('Jogador mudou de andar; acompanhamento encerrado')
        try:current=world.monster(selected['id'])
        except RatDataError as error:
            d=error.details
            same=(d.get('id')==selected['id'] and d.get('actual_id')==selected['id']
                and d.get('object')==selected['object'] and d.get('control')==selected['control'])
            if same and d.get('reason')=='invalid_position_or_health' and d.get('position')=={'x':65535,'y':65535,'z':255}:
                if isinstance(d.get('hp'),int) and 0<d['hp']<=100:
                    report.update(status='target_lost',loss_details=d);return
                if d.get('hp')==0:current=None
                else:raise
            else:raise
        if current is not None:
            if any(current.get(k)!=selected.get(k) for k in ('object','control','name')):
                raise RuntimeError('Identidade do monstro mudou durante acompanhamento')
            if current['position']['z']!=selected['position']['z']:raise SkipCycle('Alvo mudou de andar')
        if follow:follow(selected,current,player,report)
        if current is None or current['hp']==0:
            missing+=1
            if missing>=2:
                report.update(status='target_left_world',death_confirmed=False);return
        else:missing=0
        sleep(0.15)
    raise SkipCycle('Prazo de acompanhamento encerrado; nao repete ataque')
