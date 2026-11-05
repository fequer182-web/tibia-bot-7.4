"""Open an item bag or use a configured food in a freshly read open container."""
from cromin.core.paths import BIN
from cromin.actions.common import native_completed,SkipCycle
from cromin.actions.item_loot import safe_preflight


def contained_action(pid,thread,source,item,action,runner):
    if action not in ('use','open'):raise ValueError('Acao de item invalida')
    if action=='open' and item['id']!=2853:raise SkipCycle('Bag interna diferente de 2853')
    result=runner([BIN/'OTC_Item_Use_Once.exe','--pid',pid,'--thread',thread,'--action',action,
        '--source',source['container_id'],'--slot',item['slot'],'--item-id',item['id'],'--raw',item['raw_count_or_subtype'],
        '--source-item-id',source['bag_item']['id'],'--source-object',source['address'],
        '--source-bag-object',source['bag_item']['address'],'--item-object',item['address']],'[JSON] ')
    if safe_preflight(result):raise SkipCycle('Item/container mudou antes da chamada; acao ignorada')
    if not native_completed(result):raise RuntimeError('Acao de item/restauracao nao confirmada; nao repete')
    d=result['report']
    if (d.get('item_id'),d.get('borrowed_item'),d.get('source_container'),d.get('source_slot'),d.get('action'))!=(item['id'],item['address'],source['container_id'],item['slot'],'open_bag' if action=='open' else 'use_food'):
        raise RuntimeError('Acao de item retornou identidade/acao diferente')
    return result
