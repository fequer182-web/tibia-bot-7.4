"""Experimento externo de luz: um byte em LocalPlayer+0xF1, com restauracao condicionada."""
import ctypes as C
from ctypes import wintypes as W
import struct
from cromin.readers.memory import MBI

class WindowsByteWriter:
    """Abre permissao de escrita somente quando o modulo de luz precisa escrever."""
    def __init__(self,pid):
        self.api=C.WinDLL('kernel32',use_last_error=True)
        signatures={
            'OpenProcess':([W.DWORD,W.BOOL,W.DWORD],W.HANDLE),
            'CloseHandle':([W.HANDLE],W.BOOL),
            'VirtualQueryEx':([W.HANDLE,C.c_void_p,C.POINTER(MBI),C.c_size_t],C.c_size_t),
            'WriteProcessMemory':([W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.POINTER(C.c_size_t)],W.BOOL),
            'ReadProcessMemory':([W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.POINTER(C.c_size_t)],W.BOOL)}
        for name,(args,result) in signatures.items():
            fn=getattr(self.api,name);fn.argtypes=args;fn.restype=result
        self.handle=self.api.OpenProcess(0x400|0x10|0x20|0x8,False,pid)
        if not self.handle:raise C.WinError(C.get_last_error())
    def write(self,address,value):
        if not isinstance(value,int) or not 0<=value<=31:raise ValueError('Luz fora da faixa 0 a 31')
        region=MBI()
        got=self.api.VirtualQueryEx(self.handle,address,C.byref(region),C.sizeof(region))
        if got!=C.sizeof(region) or region.State!=0x1000 or region.Protect&0x100 or (region.Protect&255) not in (4,8):
            raise RuntimeError('Pagina do byte de luz nao esta gravavel')
        data=C.c_ubyte(value);count=C.c_size_t()
        if not self.api.WriteProcessMemory(self.handle,address,C.byref(data),1,C.byref(count)) or count.value!=1:
            raise RuntimeError(f'Escrita de luz nao confirmada; erro {C.get_last_error()}')
        readback=C.c_ubyte();count.value=0
        if not self.api.ReadProcessMemory(self.handle,address,C.byref(readback),1,C.byref(count)) or count.value!=1 or readback.value!=value:
            raise RuntimeError('Byte de luz nao conferiu apos a escrita')
    def close(self):
        if self.handle:self.api.CloseHandle(self.handle);self.handle=None

class ExternalLight:
    def __init__(self,memory,base,pid,record=lambda *args:None,writer_factory=WindowsByteWriter):
        self.memory,self.base,self.pid,self.record=memory,base,pid,record
        self.writer_factory=writer_factory;self.writer=None;self.active=False
        self.player=self.address=None;self.baseline=self.last_written=None
        self.status='Desligada';self.observed=None
    def sample(self):
        player=struct.unpack('<Q',self.memory.exact(self.base+0xE52A48,8))[0]
        if not 0x10000<=player<0x800000000000:raise ValueError('Ponteiro do jogador indisponivel para luz')
        if struct.unpack('<Q',self.memory.exact(player,8))[0]!=self.base+0xAE99E0:
            raise ValueError('VTable do jogador diferente para luz')
        x,y,z=struct.unpack('<IIB',self.memory.exact(player+0x30,9))
        if not (0<x<65535 and 0<y<65535 and z<=15):raise ValueError('Posicao do jogador invalida para luz')
        value=self.memory.exact(player+0xF1,1)[0]
        if value>31:raise ValueError(f'Byte candidato de luz fora da faixa esperada: {value}')
        return player,player+0xF1,value
    def write(self,value):
        if self.writer is None:self.writer=self.writer_factory(self.pid)
        self.writer.write(self.address,value)
    def tick(self,enabled,level):
        if not enabled:
            if self.active:self.disable()
            return
        if not isinstance(level,int) or not 0<=level<=31:raise ValueError('Intensidade deve estar entre 0 e 31')
        player,address,value=self.sample()
        if not self.active:
            self.player,self.address,self.baseline,self.last_written=player,address,value,value
            self.active=True
            self.record('light_enabled',{'player':hex(player),'offset':'0xF1','baseline':value,'selected':level,'visual_effect_confirmed':False})
        elif player!=self.player or address!=self.address:
            raise ValueError('Jogador mudou; luz desativada sem escrever no objeto antigo')
        if value!=self.last_written:self.baseline=value
        if value!=level:
            self.last_written=level
            self.write(level)
            self.record('light_byte_written',{'address':hex(address),'value':level,'visual_effect_confirmed':False})
        self.observed=level;self.status=f'Valor aplicado: {level}. Confira a iluminacao no jogo.'
    def disable(self):
        outcome='already_disabled'
        try:
            if not self.active:return outcome
            self.active=False
            try:player,address,value=self.sample()
            except (ValueError,RuntimeError,OSError):
                outcome='restore_skipped_player_unavailable';self.status='Desligada; jogador indisponivel para restaurar.'
                return outcome
            self.observed=value
            if player!=self.player or address!=self.address:
                outcome='restore_skipped_player_changed';self.status='Desligada; jogador mudou, restauracao ignorada.'
            elif value!=self.last_written:
                outcome='client_value_preserved';self.status=f'Desligada; valor atual do cliente preservado: {value}.'
            else:
                self.write(self.baseline);self.observed=self.baseline;outcome='restored'
                self.status=f'Desligada; byte anterior restaurado: {self.baseline}.'
            return outcome
        except Exception:
            outcome='restore_failed';self.status='Falha ao restaurar o byte de luz; consulte o relatorio.';raise
        finally:
            try:
                if outcome!='already_disabled':self.record('light_disabled',{'outcome':outcome,'baseline':self.baseline})
            finally:
                if self.writer is not None:self.writer.close();self.writer=None
    def close(self):return self.disable()
