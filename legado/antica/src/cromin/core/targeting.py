"""Target filters shared by UI, settings and the controller."""
import re

def parse_names(value):
    parts=re.split(r'[,;\n]',value) if isinstance(value,str) else value
    if not isinstance(parts,(list,tuple)) or len(parts)>32:raise ValueError('Use ate 32 nomes separados por virgula.')
    result=[];seen=set()
    for item in parts:
        if not isinstance(item,str):raise ValueError('Nome de alvo precisa ser texto.')
        name=item.strip()
        if not name:continue
        if len(name.encode('utf-8'))>128 or any(ord(c)<32 for c in name):raise ValueError('Nome de monstro invalido.')
        key=name.casefold()
        if key not in seen:seen.add(key);result.append(name)
    return result

def matches(name,names,any_monster=False):
    return bool(any_monster or name.casefold() in {n.casefold() for n in parse_names(names)})
