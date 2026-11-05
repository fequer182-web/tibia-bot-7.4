"""Configured corpse loot and corpse-only Drop, with an observed result per move."""
import time
from cromin.actions.common import native_completed,SkipCycle
from cromin.actions.loot_transfer import by_id,stable
from cromin.core.paths import BIN


def read_containers(world,memory):
    try:return stable(world,memory)
    except (ValueError,OSError) as error:raise SkipCycle('Leitura dos containers indisponivel: '+str(error)) from error


def container_at(containers,cid):
    try:return by_id(containers,cid)
    except ValueError as error:raise SkipCycle(str(error)) from error


def ground_items(world,position):
    try:return world.items_at(position)
    except (ValueError,OSError) as error:raise SkipCycle('Leitura do tile de Drop indisponivel: '+str(error)) from error


def item_map(items,item_id):
    result={}
    for item in items:
        if item['id']!=item_id:continue
        key=item.get('address',item.get('object'))
        if not key or key in result:raise SkipCycle('Identidade de item ausente ou duplicada')
        result[key]=item['raw_count_or_subtype']
    return result


def source_change(before,after,item,count):
    obj=item['address'];raw=item['raw_count_or_subtype']
    if any(after.get(k)!=v for k,v in before.items() if k!=obj) or any(k not in before for k in after):return None
    if obj not in after:
        if item['id']==3031:return ('units',count) if raw==count else None
        return ('whole',raw)
    if raw>count and after[obj]==raw-count and 1<=raw<=100:
        return ('units',count)
    return None


def destination_change(before,after,change):
    if change is None:return False
    kind,value=change
    added=[v for k,v in after.items() if k not in before]
    changed=[(v,after.get(k)) for k,v in before.items() if after.get(k)!=v]
    if len(added)==1 and not changed:return added[0]==value
    if kind=='units' and not added and len(changed)==1:
        old,new=changed[0];return new is not None and 1<=old<new<=100 and new-old==value
    # A final one-unit stack may merge; charged/fluid items with a larger raw value may not.
    if kind=='whole' and value==1 and not added and len(changed)==1:
        old,new=changed[0];return new is not None and 1<=old<new<=100 and new-old==1
    return False


def identity(container):
    return (container['container_id'],container['address'],container['bag_item']['address'],container['bag_item']['id'])


def safe_preflight(result):
    d=result.get('report') or {}
    return d.get('preflight_skipped') and not d.get('started') and d.get('detached') and not d.get('diagnostics')


def move_item(memory,world,pid,thread,source,destination,item,drop,report,stop,runner,pulse,clock=time.monotonic,sleep=time.sleep):
    count=item['raw_count_or_subtype'] if item['id']==3031 else 1
    if not 1<=count<=100:raise SkipCycle('Quantidade de moeda invalida')
    source_before=item_map(source['items'],item['id']);source_identity=identity(source)
    command=[BIN/'OTC_Loot_Once.exe','--pid',pid,'--thread',thread,'--source',source['container_id'],
             '--slot',item['slot'],'--count',count,'--item-id',item['id'],'--raw',item['raw_count_or_subtype'],
             '--source-item-id',source['bag_item']['id'],'--source-object',source['address'],
             '--source-bag-object',source['bag_item']['address'],'--item-object',item['address']]
    if drop:
        position=world.player();tile=world.tile_pair(position)
        if not tile:raise SkipCycle('Tile debaixo do jogador nao localizado; Drop ignorado')
        ground_identity=tile[1];destination_before=item_map(ground_items(world,position),item['id'])
        command+=['--ground',1,'--ground-tile',hex(ground_identity[0]),'--x',position['x'],'--y',position['y'],'--z',position['z']]
    else:
        destination_identity=identity(destination);destination_before=item_map(destination['items'],item['id'])
        fits_gold=item['id']==3031 and any(1<=raw<=100-count for raw in destination_before.values())
        if destination['item_count']>=destination['capacity'] and not fits_gold:raise SkipCycle('Bag cheia; nao transfere')
        command+=['--destination',destination['container_id'],'--destination-object',destination['address'],
                  '--destination-bag-object',destination['bag_item']['address']]
    pulse()
    if stop():raise SkipCycle('Loot desativado; nenhuma nova transferencia')
    result=runner(command,'[JSON] ')
    report.update(item_id=item['id'],count_requested=count,action='drop' if drop else 'take',native=result,confirmed=False)
    if safe_preflight(result):raise SkipCycle('Item/tile mudou antes da chamada; transferencia ignorada')
    if not native_completed(result):raise RuntimeError('Transferencia/restauracao nativa nao confirmada; nao repete')
    native=result['report']
    if native.get('item_id')!=item['id'] or native.get('borrowed_item')!=item['address']:
        raise RuntimeError('Helper moveu identidade diferente da solicitada')
    if (native.get('source_container'),native.get('source_slot'),native.get('requested_count'))!=(source['container_id'],item['slot'],count):
        raise RuntimeError('Origem/quantidade divergente no retorno da transferencia')
    if not drop and (native.get('destination',{}).get('x')!=65535 or native.get('destination',{}).get('y')!=(0x40|destination['container_id'])):
        raise RuntimeError('Destino divergiu da bag solicitada')
    expected=position if drop else None
    if drop and native.get('destination')!=expected:raise RuntimeError('Destino de Drop divergiu do SQM solicitado')
    deadline=clock()+5
    while clock()<deadline and not stop():
        pulse()
        try:
            current=stable(world,memory);src=by_id(current,source['container_id'])
            if identity(src)!=source_identity:raise SkipCycle('Corpo mudou depois da transferencia')
            delta=source_change(source_before,item_map(src['items'],item['id']),item,count)
            if drop:
                tile=world.tile_pair(position)
                if not tile or tile[1]!=ground_identity:raise SkipCycle('Tile de Drop mudou depois da chamada')
                observed=item_map(ground_items(world,position),item['id'])
            else:
                dst=by_id(current,destination['container_id'])
                if identity(dst)!=destination_identity:raise SkipCycle('Bag mudou depois da transferencia')
                observed=item_map(dst['items'],item['id'])
            if destination_change(destination_before,observed,delta):
                report.update(confirmed=True,status='drop_confirmed' if drop else 'take_confirmed',after=current)
                if drop:report['ground_position']=position
                return count
        except SkipCycle:raise
        except (ValueError,OSError):pass
        sleep(0.15)
    raise SkipCycle('Resultado da transferencia nao confirmado; corpo ignorado sem repetir chamada')


def open_configured(memory,world,pid,thread,destination,report,stop,policy,position,expected,open_only,runner,pulse):
    from cromin.actions.open_loot import pick_corpse
    if not policy.take and not policy.drop:
        report.update(status='empty_loot_lists',confirmed=True,collected=0);return
    before=read_containers(world,memory)
    if policy.take:container_at(before,destination)
    corpse=pick_corpse(world,position,expected)
    if corpse['id'] not in policy.corpses:raise SkipCycle('ID do corpo fora da lista configurada')
    if pick_corpse(world,corpse['position'],int(corpse['object'],16))!=corpse:raise SkipCycle('Corpo mudou entre leituras')
    report.update(status='opening_configured',corpse=corpse,policy=policy.settings(),moves=[],confirmed=False,collected=0,dropped=0)
    pulse()
    if stop():raise SkipCycle('Loot desativado antes da abertura')
    p=corpse['position']
    result=runner([BIN/'OTC_Corpse_Open_Once.exe','--pid',pid,'--thread',thread,'--x',p['x'],'--y',p['y'],'--z',p['z'],
                   '--object',corpse['object'],'--corpse-id',corpse['id']],'[JSON] ')
    report['native_open']=result
    if safe_preflight(result):raise SkipCycle('Corpo mudou antes da abertura')
    d=result.get('report') or {}
    if all(d.get(k) for k in ('returned','context_restored','stack_slot_restored','detached')) and int(d.get('raw_rax_at_return','0xffffffff'),16)&0xffffffff==0xffffffff:
        raise SkipCycle('Cliente recusou abertura do corpo; nao repete')
    if not native_completed(result):raise RuntimeError('Abertura/restauracao nativa nao confirmada')
    if d.get('corpse_item_id')!=corpse['id'] or d.get('corpse_object')!=corpse['object']:raise RuntimeError('Abertura retornou identidade diferente do corpo escolhido')
    cid=int(d['raw_rax_at_return'],16)&0xffffffff
    if not 0<=cid<=63 or any(c['container_id']==cid for c in before) or (policy.take and cid==destination):raise SkipCycle('Abertura nao retornou janela nova')
    deadline=time.monotonic()+5;source=None
    while time.monotonic()<deadline and not stop():
        pulse()
        try:
            sample=stable(world,memory);candidate=by_id(sample,cid)
            if candidate['bag_item']['id']==corpse['id']:source=candidate;break
        except (ValueError,OSError):pass
        time.sleep(0.15)
    if source is None:raise SkipCycle('Janela do corpo nao apareceu; nao transfere')
    dest_identity=identity(container_at(sample,destination)) if policy.take else None
    report.update(open_confirmed=True,source_container=cid,status='configured_loot',opened_bags=[])
    if open_only:report.update(confirmed=True);return
    visited=set();budget=[0]
    def process_container(container,depth):
        original=identity(container);container_id=container['container_id']
        while budget[0]<512:
            pulse()
            if stop():raise SkipCycle('Loot desativado')
            current=read_containers(world,memory);src=container_at(current,container_id)
            if identity(src)!=original:raise SkipCycle('Container de loot mudou; ciclo ignorado')
            nested=next((i for i in src['items'] if i['id']==2853 and i['address'] not in visited),None)
            if nested is not None:
                visited.add(nested['address'])
                if depth>=4 or len(report['opened_bags'])>=16:
                    report.setdefault('nested_skipped',[]).append({'item':nested['address'],'reason':'depth_or_bag_limit'})
                else:
                    from cromin.actions.contained_item import contained_action
                    budget[0]+=1
                    opened=contained_action(pid,thread,src,nested,'open',runner)
                    child_id=int(opened['report']['raw_rax_at_return'],16)&0xffffffff
                    if not 0<=child_id<=63 or any(c['container_id']==child_id for c in current):raise SkipCycle('Bag interna nao retornou uma nova janela; nao repete abertura')
                    deadline=time.monotonic()+5;child=None
                    while time.monotonic()<deadline and not stop():
                        pulse()
                        try:
                            candidate=by_id(stable(world,memory),child_id)
                            if candidate['bag_item']['id']==2853:child=candidate;break
                        except (ValueError,OSError):pass
                        time.sleep(0.15)
                    if child is None:raise SkipCycle('Bag 2853 nao apareceu apos abertura; nao repete')
                    if dest_identity is not None and identity(child)==dest_identity:raise SkipCycle('Bag do monstro coincide com destino; nao transfere')
                    report['opened_bags'].append({'parent':container_id,'container':child_id,'item_object':nested['address'],'native':opened})
                    process_container(child,depth+1)
                    continue
            dst=container_at(current,destination) if policy.take else None
            if dst and identity(dst)!=dest_identity:raise SkipCycle('Bag de destino mudou')
            choices=[i for i in src['items'] if i['id'] in policy.take or i['id'] in policy.drop]
            if not choices:return
            choices.sort(key=lambda i:i['id'] not in policy.drop)
            item=choices[0];drop=item['id'] in policy.drop;move={};report['moves'].append(move)
            budget[0]+=1
            count=move_item(memory,world,pid,thread,src,dst,item,drop,move,stop,runner,pulse)
            key='dropped' if drop else 'collected';report[key]+=count
        raise SkipCycle('Limite de 512 acoes de loot por corpo atingido')
    process_container(source,0)
    report.update(status='configured_loot_confirmed',confirmed=True,after=read_containers(world,memory))
