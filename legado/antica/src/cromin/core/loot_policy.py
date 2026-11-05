"""Validated item ID lists shared by UI, profiles and the loot scheduler."""
import re
from dataclasses import dataclass

def parse_ids(value):
    if isinstance(value,str):
        if len(value)>4096:raise ValueError('Lista de IDs muito longa')
        values=[v for v in re.split(r'[,;\s]+',value.strip()) if v]
    elif isinstance(value,(list,tuple)):values=value
    else:raise ValueError('IDs precisam ser uma lista ou texto separado por virgula')
    result=[]
    for item in values:
        if type(item) is int:number=item
        elif isinstance(item,str) and item.isascii() and item.isdecimal():number=int(item)
        else:raise ValueError('Use somente IDs numericos inteiros')
        if not 1<=number<=65535:raise ValueError('IDs precisam estar entre 1 e 65535')
        if number not in result:result.append(number)
    if len(result)>128:raise ValueError('Limite de 128 IDs por lista')
    return result

@dataclass(frozen=True)
class LootPolicy:
    corpses:tuple=(3994,)
    take:tuple=(3031,)
    drop:tuple=()
    drop_scope:str='corpse'
    @classmethod
    def create(cls,corpses=(3994,),take=(3031,),drop=(),drop_scope='corpse'):
        c,t,d=map(parse_ids,(corpses,take,drop))
        if set(t)&set(d):raise ValueError('Um ID nao pode estar em Loot e Drop ao mesmo tempo')
        if drop_scope!='corpse':raise ValueError('Origem do Drop invalida')
        return cls(tuple(c),tuple(t),tuple(d),drop_scope)
    def settings(self):return {'corpse_ids':list(self.corpses),'loot_ids':list(self.take),'drop_ids':list(self.drop),'drop_scope':self.drop_scope}
    @classmethod
    def from_state(cls,s):return cls.create(s.corpseIds,s.lootIds,s.dropIds,s.dropScope)
