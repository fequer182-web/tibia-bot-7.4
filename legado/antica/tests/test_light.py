from pathlib import Path
import ctypes as C,os,struct,sys
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'src'))
from cromin.actions.light import ExternalLight,WindowsByteWriter
from cromin.core.state import Bridge,LIGHT,ATTACK,LOOP
BASE=0x140000000;PLAYER=0x220000
class FakeMemory:
    def __init__(self):self.data={};self.reads=0
    def put(self,address,fmt,*values):
        self.data.update({address+i:v for i,v in enumerate(struct.pack(fmt,*values))})
    def exact(self,address,size):
        self.reads+=1
        try:return bytes(self.data[address+i] for i in range(size))
        except KeyError:raise RuntimeError('unreadable')
    def player(self,address):
        self.put(BASE+0xE52A48,'Q',address);self.put(address,'Q',BASE+0xAE99E0)
        self.put(address+0x30,'<IIB',100,101,7);self.put(address+0xF1,'B',3)

m=FakeMemory();m.player(PLAYER);writes=[];writers=[];events=[];fail=[False]
class FakeWriter:
    def __init__(self,pid):self.closed=False;writers.append(self)
    def write(self,address,value):
        assert address in (PLAYER+0xF1,0x230000+0xF1) and 0<=value<=31
        writes.append((address,value));m.put(address,'B',value)
        if fail[0]:fail[0]=False;raise RuntimeError('readback failed after writing')
    def close(self):self.closed=True
light=ExternalLight(m,BASE,1,lambda *data:events.append(data),FakeWriter)
light.tick(False,7);assert not writers and not m.reads
light.tick(True,7);light.tick(True,7);assert len(writes)==1 and m.exact(PLAYER+0xF1,1)==b'\x07'
light.tick(True,10);assert light.disable()=='restored' and m.exact(PLAYER+0xF1,1)==b'\x03' and writers[-1].closed
light.tick(True,7);m.put(PLAYER+0xF1,'B',6);light.tick(True,7)
assert light.disable()=='restored' and m.exact(PLAYER+0xF1,1)==b'\x06'
light.tick(True,7);m.put(PLAYER+0xF1,'B',4);before=len(writes)
assert light.disable()=='client_value_preserved' and len(writes)==before
light.tick(True,7);m.player(0x230000);before=len(writes)
try:light.tick(True,7)
except ValueError:pass
else:raise AssertionError('Player change accepted')
assert light.disable()=='restore_skipped_player_changed' and len(writes)==before and writers[-1].closed
m.player(PLAYER);m.put(PLAYER+0xF1,'B',255);before=len(writes)
try:light.tick(True,7)
except ValueError:pass
else:raise AssertionError('Invalid candidate accepted')
assert len(writes)==before
m.player(PLAYER);fail[0]=True
try:light.tick(True,7)
except RuntimeError:pass
else:raise AssertionError('Unconfirmed write accepted')
assert light.disable()=='restored' and m.exact(PLAYER+0xF1,1)==b'\x03'
b=Bridge()
with b.lock() as s:s.flags=LIGHT|ATTACK|LOOP
b.disable();assert b.snapshot().flags==LOOP
print('PASS: luz inicia desligada, um byte, faixa, restaura, preserva atualizacao do cliente, troca de jogador e rollback')

# Teste WinAPI real somente em memoria alocada por este processo Python.
k=C.WinDLL('kernel32',use_last_error=True)
k.VirtualAlloc.argtypes=[C.c_void_p,C.c_size_t,C.c_uint32,C.c_uint32];k.VirtualAlloc.restype=C.c_void_p
k.VirtualProtect.argtypes=[C.c_void_p,C.c_size_t,C.c_uint32,C.POINTER(C.c_uint32)];k.VirtualProtect.restype=C.c_int
k.VirtualFree.argtypes=[C.c_void_p,C.c_size_t,C.c_uint32];k.VirtualFree.restype=C.c_int
address=k.VirtualAlloc(None,4096,0x3000,4);assert address
writer=WindowsByteWriter(os.getpid())
try:
    C.memset(address,3,4096);writer.write(address+0xF1,7)
    assert C.string_at(address+0xF0,3)==b'\x03\x07\x03'
    previous=C.c_uint32();assert k.VirtualProtect(address,4096,2,C.byref(previous))
    try:writer.write(address+0xF1,9)
    except RuntimeError:pass
    else:raise AssertionError('Read-only page accepted')
    assert C.string_at(address+0xF1,1)==b'\x07'
finally:
    writer.close();k.VirtualFree(address,0,0x8000)
print('PASS: WriteProcessMemory de um byte e rejeicao de pagina somente leitura em processo de teste proprio')
