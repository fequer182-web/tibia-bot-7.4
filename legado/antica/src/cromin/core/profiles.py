"""Configuration codec. These are Cromin settings, not Crome ABI/build profiles."""
from cromin.core.targeting import parse_names
from cromin.core.loot_policy import LootPolicy,parse_ids

def validate_profile(data):
    if not isinstance(data,dict) or not {'hp','cooldown','destination','spell'}<=data.keys():
        raise ValueError('Selecione uma configuracao do Cromin com hp, cooldown, destination e spell.')
    hp=int(data['hp']);cooldown=int(data['cooldown']);dest=int(data['destination']);light=31
    spell=data['spell'];points=data.get('waypoints',[])
    if not (1<=hp<=1000000 and 1000<=cooldown<=60000 and 0<=dest<=63 and 0<=light<=31):
        raise ValueError('HP, intervalo, bag ou luz fora dos limites.')
    if not isinstance(spell,str) or len(spell)>15 or any(not 32<=ord(c)<=126 for c in spell):
        raise ValueError('Magia invalida: ate 15 caracteres ASCII.')
    if not isinstance(points,list) or len(points)>64:raise ValueError('Rota invalida ou acima de 64 pontos.')
    route=[]
    for p in points:
        if not isinstance(p,dict) or not all(type(p.get(k)) is int for k in ('x','y','z')):
            raise ValueError('Coordenadas invalidas na rota.')
        if not (0<p['x']<65535 and 0<p['y']<65535 and 0<=p['z']<=15):raise ValueError('Coordenadas fora dos limites.')
        route.append({k:p[k] for k in ('x','y','z')})
    names=parse_names(data.get('target_names',['Rat']));any_monster=data.get('target_any',False)
    if type(any_monster) is not bool or (not names and not any_monster):raise ValueError('Filtro de alvo invalido.')
    follow=data.get('target_follow',True)
    if type(follow) is not bool:raise ValueError('Follow precisa ser booleano.')
    policy=LootPolicy.create(data.get('corpse_ids',[3994]),data.get('loot_ids',[3031]),data.get('drop_ids',[]),data.get('drop_scope','corpse'))
    return dict(food_ids=parse_ids(data.get('food_ids',[3577])),**policy.settings(),target_follow=follow,target_names=names,target_any=any_monster,hp=hp,cooldown=cooldown,destination=dest,spell=spell,light=light,loop=True,waypoints=route)
