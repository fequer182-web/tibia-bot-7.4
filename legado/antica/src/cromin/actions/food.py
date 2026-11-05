"""Periodic use of explicitly configured food IDs in open player/monster containers."""
import time
from cromin.actions.loot_transfer import stable,by_id
from cromin.actions.common import SkipCycle
from cromin.actions.item_loot import identity,item_map,source_change
from cromin.actions.contained_item import contained_action

class AutoFood:
    def __init__(self,memory,world,pid,thread,runner,record=lambda *a:None,clock=time.monotonic,sleep=time.sleep):
        self.memory,self.world,self.pid,self.thread=memory,world,pid,thread
        self.runner,self.record,self.clock,self.sleep=runner,record,clock,sleep
        self.last=-1e9;self.blocked={};self.status='Auto Food desligado';self.busy=False
    def tick(self,enabled,ids,stop=lambda:False):
        if not enabled:self.status='Auto Food desligado';return False
        if self.busy or self.clock()-self.last<15:return False
        if not ids:self.status='Auto Food: informe IDs de food';return False
        containers=stable(self.world,self.memory);now=self.clock()
        choices=[(c,i) for c in containers for i in c['items'] if i['id'] in ids and self.blocked.get((c['address'],i['address']),0)<=now]
        if not choices:self.status='Auto Food: sem food disponivel nas bags abertas';return False
        source,item=choices[0];key=(source['address'],item['address']);before=item_map(source['items'],item['id']);original=identity(source)
        if stop():return False
        self.last=now;self.busy=True;report={'source':source['container_id'],'item_id':item['id'],'item_object':item['address'],'confirmed':False}
        try:
            result=contained_action(self.pid,self.thread,source,item,'use',self.runner);report['native']=result
            deadline=self.clock()+2
            while self.clock()<deadline and not stop():
                try:
                    current=by_id(stable(self.world,self.memory),source['container_id'])
                    if identity(current)!=original:raise SkipCycle('Container mudou apos uso de food')
                    delta=source_change(before,item_map(current['items'],item['id']),item,1)
                    if delta is not None:
                        report.update(status='food_item_change_confirmed',confirmed=True);self.status=f"Auto Food: uso do ID {item['id']} confirmado";return True
                except SkipCycle:pass
                except (ValueError,OSError):pass
                self.sleep(0.15)
            self.blocked[key]=self.clock()+60;self.last=self.clock()+45;report['status']='food_not_consumed_backoff'
            self.status='Auto Food: consumo nao observado; aguardando antes de tentar novamente';return True
        except SkipCycle as error:
            self.blocked[key]=self.clock()+60;report.update(status='food_read_skipped',reason=str(error));self.status='Auto Food: item mudou; tentativa ignorada';return False
        finally:
            self.busy=False;self.record('auto_food',report)
