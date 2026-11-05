"""Leitores Ancientia desta build para o painel de acao."""
import struct
from cromin.readers.ancientia import read_pointer
from cromin.readers.ancientia_pending import snapshot,decode_name
from cromin.readers.memory import blocks

from cromin.readers.ancientia_profile import GAME_RVA,MONSTER_VTABLE_RVA,VTABLE_RVA,READER_SIGNATURES
LOCAL_PLAYER_VTABLE_RVA=VTABLE_RVA


def validate_action_readers(memory,base):
    # Bodies dos getters desta build: getLocalPlayer ECX+30,
    # getAttackingCreature ECX+34 e getFollowingCreature ECX+38.
    for rva,code in READER_SIGNATURES:
        expected=bytes.fromhex(code)
        if memory.exact(base+rva,len(expected))!=expected:
            raise RuntimeError(f'Getter da nova build divergente em {hex(rva)}; acao recusada.')


def read_state(memory,base):
    try:current,_=snapshot(memory,base)
    except (OSError,RuntimeError) as error:
        raise RuntimeError(f'Leitura do jogador: {error}') from error
    position=current['positions']['0xc']
    xyz=(position['x'],position['y'],position['z'])
    if not (0<xyz[0]<65535 and 0<xyz[1]<65535 and 0<=xyz[2]<=15):
        raise RuntimeError('Posicao invalida; acao interrompida.')
    target_address=read_pointer(memory,base+GAME_RVA+0x34)
    target=None
    if target_address:
        if not 0x10000<=target_address<=0xFFFFFFFF-0x80:
            raise RuntimeError('Ponteiro do alvo invalido')
        try:raw=memory.exact(target_address,0x80)
        except (OSError,RuntimeError) as error:
            raise RuntimeError(f'Leitura do alvo {hex(target_address)}: {error}') from error
        vt=struct.unpack_from('<I',raw)[0]
        if vt!=base+MONSTER_VTABLE_RVA:
            target=dict(address=hex(target_address),className='Other',hpPercent=0)
        else:
            hp=raw[0x5C]
            if hp>100:raise RuntimeError('HP percentual do monstro invalido')
            target=dict(address=hex(target_address),className='Monster',hpPercent=hp,
                        name=decode_name(memory,raw,0x44),
                        position=struct.unpack_from('<III',raw,0xC))
        if read_pointer(memory,base+GAME_RVA+0x34)!=target_address:
            raise RuntimeError('Alvo mudou durante a leitura; tente novamente.')
    if read_pointer(memory,base+GAME_RVA+0x30)!=int(current['objectAddress'],0):
        raise RuntimeError('Jogador mudou durante a leitura.')
    return dict(player=current['objectAddress'],name=current['names']['0x44'],
                health=current['known']['health'],maxHealth=current['known']['maxHealth'],
                mana=current['known']['mana'],position=xyz,target=target)


def light_candidates(states):
    """OFF/ON/OFF/ON, duas leituras por estado. Somente evidencias, sem escrita."""
    if len(states)!=4:raise ValueError('Sao necessarios quatro estados.')
    if len({s['player'] for s in states})!=1 or len({tuple(s['position']) for s in states})!=1:
        raise ValueError('Jogador/posicao mudou; repita parado no mesmo SQM.')
    arrays=[(bytes.fromhex(s['first']),bytes.fromhex(s['second'])) for s in states]
    if any(len(a)!=0x500 or len(b)!=0x500 for a,b in arrays):
        raise ValueError('Captura de luz incompleta')
    hits=[]
    for i in range(0x80,0x500):
        if any(a[i]!=b[i] for a,b in arrays):continue
        a,b,c,d=(pair[0][i] for pair in arrays)
        if a==c and b==d and a!=b:
            hits.append(dict(offset=hex(i),off=a,on=b,
                             levelCandidate=0<=a<b<=31,status='repeated_transition_candidate'))
    return hits


def read_monster(memory,base,address,allow_dead=False):
    if not 0x10000<=address<=0xFFFFFFFF-0x80:return None
    raw=memory.exact(address,0x80)
    if struct.unpack_from('<I',raw)[0]!=base+MONSTER_VTABLE_RVA:return None
    x,y,z=struct.unpack_from('<III',raw,0xC)
    hp=raw[0x5C]
    name=decode_name(memory,raw,0x44)
    if not (0<x<65535 and 0<y<65535 and 0<=z<=15 and (0 if allow_dead else 1)<=hp<=100 and name):return None
    if memory.exact(address,4)!=raw[:4]:return None
    return dict(address=hex(address),name=name,position=(x,y,z),hpPercent=hp,className='Monster')


def scan_monsters(memory,base,cancel=lambda:False,progress=lambda count,stats:None):
    pattern=struct.pack('<I',base+MONSTER_VTABLE_RVA)
    stats=dict(read_bytes=0,failed_bytes=0,failed_pages=0,failure_examples=[],regions=0)
    found={}
    for address,size in memory.regions('writable'):
        if cancel():return [],dict(stats,cancelled=True)
        if address>=0x100000000:break
        size=min(size,0x100000000-address);stats['regions']+=1
        for start,data in blocks(memory,address,size,stats):
            if cancel():return [],dict(stats,cancelled=True)
            if data is None:continue
            offset=data.find(pattern)
            while offset>=0:
                obj=start+offset
                if obj%4==0:
                    try:
                        monster=read_monster(memory,base,obj)
                        if monster:found[obj]=monster
                    except (RuntimeError,OSError):pass
                if len(found)>8192:raise ValueError('Instancias demais; varredura interrompida.')
                offset=data.find(pattern,offset+1)
        progress(len(found),stats);memory.alive()
    return list(found.values()),dict(stats,cancelled=False)
