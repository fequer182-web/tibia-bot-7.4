"""Monitor externo e refinamento conjunto no LocalPlayer conhecido."""
import math
import struct
from decimal import Decimal
from cromin.readers.ancientia import read_pointer, validate_target
from cromin.readers.build import parse_pe

from cromin.readers.ancientia_profile import GLOBAL_RVA,VTABLE_RVA
KNOWN = {'health':0x588,'maxHealth':0x590,'mana':0x5C0,'experience':0x5A8,'level':0x5B0}
FORMATS = {'double':'<d','uint32':'<I','uint16':'<H'}
SPECS = {'maxMana': [('double',1)], 'levelPercent':[('double',1),('uint32',1)],
         'capacity':[('double',1),('double',100)],
         'magicLevel':[('double',1),('uint32',1)],
         'soul':[('double',1),('uint32',1)],
         'staminaMinutes':[('double',1),('uint32',1),('uint16',1)]}


def connect(memory):
    path = memory.executable()
    base,size = memory.main_module(path)
    def header(offset,amount):
        if not 0 <= offset <= size-amount:
            raise ValueError('Cabecalho fora do modulo')
        return memory.exact(base+offset,amount)
    pe = parse_pe(header)
    validate_target(memory,path,base,size,pe)
    return base


def decode_name(memory, blob, offset):
    length,capacity = struct.unpack_from('<II',blob,offset+16)
    if not 0 < length <= 128 or not length <= capacity <= 4096:
        return None
    if capacity == 15 and length <= 15:
        raw = blob[offset:offset+length]
    elif capacity >= 16:
        pointer = struct.unpack_from('<I',blob,offset)[0]
        if not 0x10000 <= pointer <= 0xFFFFFFFF-length:
            return None
        raw = memory.exact(pointer,length)
    else:
        return None
    return raw.decode('utf-8',errors='replace')


def read_player_prefix(memory,obj,discovery=False):
    # O ultimo campo necessario termina em 0x5D0. O bloco antigo de 0x1000
    # podia atravessar uma pagina indisponivel depois do fim do objeto.
    required=0x5D0
    data=memory.exact(obj,required)
    if len(data)!=required:raise RuntimeError('Leitura obrigatoria do jogador incompleta')
    tail_error=None
    if discovery:
        while len(data)<0x1000:
            address=obj+len(data)
            amount=min(0x1000-len(data),4096-address%4096)
            try:
                chunk=memory.exact(address,amount)
                if len(chunk)!=amount:raise RuntimeError('Extensao opcional incompleta')
            except (OSError,RuntimeError) as error:
                tail_error=str(error);break
            data+=chunk
    return data,tail_error


def snapshot(memory,base,discovery=False):
    memory.alive()
    obj = read_pointer(memory,base+GLOBAL_RVA)
    if not 0x10000 <= obj <= 0xFFFFFFFF-0x5D0:
        raise RuntimeError('Jogador indisponivel; entre no personagem e tente novamente.')
    blob,tail_error = read_player_prefix(memory,obj,discovery)
    if struct.unpack_from('<I',blob)[0] != base+VTABLE_RVA:
        raise RuntimeError('Vtable do LocalPlayer divergente. Leitura descartada.')
    names = {hex(offset):decode_name(memory,blob,offset) for offset in (0x44,0x148)}
    if (read_pointer(memory,base+GLOBAL_RVA) != obj or
        read_pointer(memory,obj) != base+VTABLE_RVA):
        raise RuntimeError('Jogador mudou durante a leitura. Leitura descartada.')
    fields = {name:struct.unpack_from('<d',blob,offset)[0] for name,offset in KNOWN.items()}
    if any(not math.isfinite(v) or v < 0 for v in fields.values()):
        raise RuntimeError('Atributo invalido; leitura descartada.')
    if fields['level'] < 1 or fields['maxHealth'] < 1 or fields['health'] > fields['maxHealth']:
        raise RuntimeError('Valores conhecidos inconsistentes; leitura descartada.')
    positions = {hex(offset):dict(zip(('x','y','z'),struct.unpack_from('<III',blob,offset)))
                 for offset in (0xC,0x3FC)}
    return dict(objectAddress=hex(obj),names=names,positions=positions,known=fields,
                readableObjectBytes=len(blob),optionalTailError=tail_error,
                maxManaCandidates={hex(off):struct.unpack_from('<d',blob,off)[0] for off in (0x5B8,0x5C8)}),blob


def candidate_matches(blob,field,observed):
    hits = []
    # Campos conhecidos nao sao promovidos novamente como outro atributo.
    occupied = [(off,off+8) for off in KNOWN.values()]
    for kind,scale in SPECS[field]:
        fmt = FORMATS[kind]
        size = struct.calcsize(fmt)
        for offset in range(0,len(blob)-size+1,2 if kind == 'uint16' else 4):
            if any(offset < end and offset+size > start for start,end in occupied):
                continue
            value = struct.unpack_from(fmt,blob,offset)[0]
            expected = float(Decimal(str(observed))*scale)
            if math.isfinite(value) and value == expected:
                hits.append(dict(offset=hex(offset),type=kind,storedScale=scale))
    return hits


def refine_field(previous,blob,field,observed):
    all_hits = candidate_matches(blob,field,observed)
    if previous and previous['status'] != 'too_many_candidates':
        hits = [c for c in previous['candidates'] if c in all_hits]
        samples = previous['observations']+[observed]
    else:
        hits,samples = all_hits,[observed]
    changes = sum(a != b for a,b in zip(samples,samples[1:]))
    if len(hits)>256:
        return dict(status='too_many_candidates',candidateCount=len(hits),candidates=[],
                    observations=samples,transitions=changes)
    status = ('no_candidates' if not hits else 'unique_with_transition' if len(hits)==1 and changes
              else 'unique_snapshot_only' if len(hits)==1 else 'ambiguous')
    return dict(status=status,candidateCount=len(hits),candidates=hits,observations=samples,transitions=changes)
