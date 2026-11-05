"""Luz Ancientia DX x86; intensidade C9, cor C8 preservada."""
from cromin.actions.light import ExternalLight,WindowsByteWriter
from cromin.readers.ancientia import read_pointer
from cromin.readers.ancientia_pending import VTABLE_RVA,GLOBAL_RVA
from cromin.readers.ancientia_pending import decode_name
from cromin.readers.ancientia_profile import LIGHT_WRITE_ALLOWED


class AncientiaLight(ExternalLight):
    def __init__(self,memory,base,pid,record=lambda *args:None,writer_factory=WindowsByteWriter):
        self.bound_identity=None
        def corrected(event,data):
            data=dict(data)
            if 'offset' in data:data['offset']='0xC9'
            data['client']='AncientiaDX x86'
            record(event,data)
        super().__init__(memory,base,pid,corrected,writer_factory)

    def sample(self):
        if not LIGHT_WRITE_ALLOWED:raise ValueError('Teste de luz bloqueado para esta build.')
        player=read_pointer(self.memory,self.base+GLOBAL_RVA)
        if not 0x10000<=player<=0xFFFFFF36:
            raise ValueError('Jogador indisponivel para luz')
        if read_pointer(self.memory,player)!=self.base+VTABLE_RVA:
            raise ValueError('Vtable Ancientia divergente para luz')
        name=decode_name(self.memory,self.memory.exact(player+0x44,24),0)
        if not name:raise ValueError('Nome indisponivel para luz')
        identity=(player,name)
        if self.bound_identity is not None and self.bound_identity!=identity:
            raise ValueError('Personagem mudou; nao escreveu/restaurou luz no novo personagem')
        self.bound_identity=identity
        value=self.memory.exact(player+0xC9,1)[0]
        if value>31:raise ValueError('Intensidade atual fora da faixa esperada; nao escreveu')
        if read_pointer(self.memory,self.base+GLOBAL_RVA)!=player:
            raise ValueError('Jogador mudou durante leitura da luz')
        return player,player+0xC9,value

    def disable(self):
        try:return super().disable()
        finally:self.bound_identity=None
