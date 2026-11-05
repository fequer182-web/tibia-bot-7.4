"""Leituras do mapa da build Antica atual, sem chamadas internas."""
import struct
from bag_read import BagReader


class RatDataError(ValueError):
    def __init__(self,details):
        self.details=details
        super().__init__('Rat rejeitado: '+str(details))


class WorldReader(BagReader):
    MAP_RVA = 0xE52E30

    def __init__(self,memory,base):
        super().__init__(memory,base)
        self.rat_diagnostics=[]
        self.rat_rejected_count=0

    def record_rejected(self,details):
        self.rat_rejected_count+=1
        if len(self.rat_diagnostics)<32:self.rat_diagnostics.append(details)

    def player(self):
        pointer = self.u64(self.base+0xE52A48)
        if self.u64(pointer) != self.base+0xAE99E0:
            raise ValueError('VTable do jogador diferente')
        x,y,z = struct.unpack('<IIH',self.read(pointer+0x30,10))
        if not (0 < x < 65535 and 0 < y < 65535 and 0 <= z <= 15):
            raise ValueError('Posicao do jogador invalida')
        return {'x':x,'y':y,'z':z}

    def tree_find(self, tree, key):
        header = self.u64(tree)
        if self.read(header+0x19,1) != b'\x01':
            raise ValueError('Sentinela do mapa divergente')
        node = self.u64(header+8)
        seen = set()
        while node != header:
            if not node or node in seen or len(seen) >= 64:
                raise ValueError('Busca no mapa ciclica/invalida')
            seen.add(node)
            raw = self.read(node,0x28)
            if raw[0x19]:raise ValueError('No inesperado no mapa')
            current = struct.unpack_from('<I',raw,0x20)[0]
            if key == current:return node
            node = struct.unpack_from('<Q',raw,0 if key < current else 0x10)[0]
        return None

    def tile_pair(self, position):
        x,y,z = (position[k] for k in ('x','y','z'))
        if not (0 < x < 65535 and 0 < y < 65535 and 0 <= z <= 15):
            raise ValueError('Coordenadas invalidas')
        key = (y>>5)*2048+(x>>5)
        node = self.tree_find(self.base+self.MAP_RVA+z*16,key)
        if not node:return None
        slot = node+0x28+(((y&31)*32+(x&31))*16)
        pair = struct.unpack('<QQ',self.read(slot,16))
        if not pair[0]:
            if pair[1]:raise ValueError('Tile nulo com controle nao nulo')
            return None
        return slot,pair

    def corpses_at(self, position):
        entry = self.tile_pair(position)
        if not entry:return []
        slot,pair = entry
        tile = pair[0]
        if self.u64(tile) != self.base+0xAF6848:
            raise ValueError('VTable do Tile diferente')
        meta = self.read(tile+0x50,24)
        begin,end,capacity = struct.unpack('<QQQ',meta)
        if end < begin or capacity < end or (end-begin)%16 or end-begin > 256*16 or capacity-begin > 65536:
            raise ValueError('Vetor de Things fora dos limites')
        result=[]
        for index in range((end-begin)//16):
            obj,control = struct.unpack('<QQ',self.read(begin+index*16,16))
            if not obj:continue
            if self.u64(obj) != self.base+0xAE9BC0:continue
            raw = self.read(obj,0x48)
            if struct.unpack_from('<H',raw,0x40)[0] != 3994:continue
            actual = dict(zip(('x','y','z'),struct.unpack_from('<IIH',raw,0x20)))
            if not control or actual != position:
                raise ValueError('Corpo nao corresponde a sua posicao no Tile')
            result.append({'object':hex(obj),'control':hex(control),'tile':hex(tile),'tile_slot':index,
                           'id':3994,'position':actual})
        if self.read(tile+0x50,24) != meta or struct.unpack('<QQ',self.read(slot,16)) != pair:
            raise ValueError('Tile mudou durante leitura')
        return result

    def nearby_corpses(self, center=None):
        center = center or self.player()
        result=[]
        for dy in (-1,0,1):
            for dx in (-1,0,1):
                position={'x':center['x']+dx,'y':center['y']+dy,'z':center['z']}
                if 0 < position['x'] < 65535 and 0 < position['y'] < 65535:
                    result.extend(self.corpses_at(position))
        return result

    def rat(self, creature_id):
        node = self.tree_find(self.base+self.MAP_RVA+0x100,creature_id)
        if node is None:return None
        obj,control = struct.unpack('<QQ',self.read(node+0x28,16))
        if not obj:
            if control:raise ValueError('Criatura nula com controle nao nulo')
            return None
        raw=self.read(obj,0x70)
        vt=struct.unpack_from('<Q',raw)[0]
        current=struct.unpack_from('<I',raw,0x40)[0]
        length,capacity=struct.unpack_from('<QQ',raw,0x58)
        position=dict(zip(('x','y','z'),struct.unpack_from('<IIH',raw,0x20)))
        details={'id':creature_id,'object':hex(obj),'control':hex(control),'actual_id':current,
                 'vtable':hex(vt),'position':position,'hp':raw[0x69],
                 'position_raw_hex':raw[0x20:0x2c].hex(),'name_length':length,'name_capacity':capacity}
        if vt != self.base+0xAF61D8 or current != creature_id or length != 3 or capacity != 15 or raw[0x48:0x4c] != b'Rat\0':
            raise RatDataError(dict(details,reason='identity_changed'))
        if not control or self.read(node+0x28,16)!=struct.pack('<QQ',obj,control):
            raise RatDataError(dict(details,reason='pointer_pair_changed_or_null_control'))
        if not (0 < position['x'] < 65535 and 0 < position['y'] < 65535 and position['z'] <= 15 and raw[0x69] <= 100):
            raise RatDataError(dict(details,reason='invalid_position_or_health'))
        return {'id':current,'object':hex(obj),'control':hex(control),'position':position,'hp':raw[0x69]}

    def rats_on_floor(self):
        return self.nearby_rats(adjacent_only=False)

    def nearby_rats(self,adjacent_only=True):
        self.rat_diagnostics=[];self.rat_rejected_count=0
        tree=self.base+self.MAP_RVA+0x100
        header,count=struct.unpack('<QQ',self.read(tree,16))
        if count>8192 or self.read(header+0x19,1)!=b'\x01':
            raise ValueError('Mapa de criaturas fora dos limites')
        stack=[self.u64(header+8)];seen=set();result=[];player=self.player()
        while stack:
            node=stack.pop()
            if node==header:continue
            if not node or node in seen or len(seen)>=count:
                raise ValueError('Mapa de criaturas ciclico/alterado')
            seen.add(node);raw=self.read(node,0x38)
            if raw[0x19]:raise ValueError('No inesperado no mapa de criaturas')
            stack.extend(struct.unpack_from('<Q',raw,offset)[0] for offset in (0,0x10))
            obj,control=struct.unpack_from('<QQ',raw,0x28)
            if not obj:
                if control:raise ValueError('Criatura nula com controle nao nulo')
                continue
            creature_id=struct.unpack_from('<I',raw,0x20)[0]
            try:
                if self.u64(obj)!=self.base+0xAF61D8:continue
                data=self.read(obj+0x48,32)
                if data[:4]!=b'Rat\0' or struct.unpack_from('<QQ',data,16)!=(3,15):continue
                rat=self.rat(creature_id)
            except RatDataError as error:
                self.record_rejected(error.details);continue
            except (OSError,RuntimeError) as error:
                self.record_rejected({'id':creature_id,'object':hex(obj),'reason':'unreadable_candidate','error':str(error)})
                continue
            if rat is None:
                self.record_rejected({'id':creature_id,'object':hex(obj),'reason':'candidate_disappeared'})
                continue
            p=rat['position']
            if rat['hp'] and p['z']==player['z'] and (not adjacent_only or max(abs(p['x']-player['x']),abs(p['y']-player['y']))<=1):
                result.append(rat)
        if len(seen)!=count or self.read(tree,16)!=struct.pack('<QQ',header,count):
            raise ValueError('Mapa de criaturas mudou durante leitura')
        return sorted(result,key=lambda rat:(max(abs(rat['position']['x']-player['x']),abs(rat['position']['y']-player['y'])),rat['id']))
