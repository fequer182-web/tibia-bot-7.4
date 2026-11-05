"""Contrato de memoria compartilhada: a DLL desenha; o controlador executa as acoes."""
import ctypes as C
from ctypes import wintypes as W
import contextlib
import time

MAGIC=0x49555243
ATTACK,LOOT,CAVE,HEAL,LOOP=1,2,4,8,16
class Position(C.Structure):
    _fields_=[('x',C.c_uint32),('y',C.c_uint32),('z',C.c_uint32)]
class State(C.Structure):
    _fields_=[(name,C.c_uint32) for name in ('magic','version','bytes','uiReady','uiClosed','heartbeat',
                'flags','hpThreshold','healCooldownMs','destination')]+[
        ('targetName',C.c_wchar*32),('healText',C.c_wchar*64),('status',C.c_wchar*256),('detail',C.c_wchar*256)]+[
        (name,C.c_uint32) for name in ('waypointCount','waypointIndex','addSerial','clearSerial','removeSerial','removeIndex')]+[
        ('waypoints',Position*64)]
assert C.sizeof(State)==2048

class Bridge:
    def __init__(self,pid):
        self.api=C.WinDLL('kernel32',use_last_error=True)
        specs={
            'CreateMutexW':([C.c_void_p,W.BOOL,W.LPCWSTR],W.HANDLE),
            'CreateFileMappingW':([W.HANDLE,C.c_void_p,W.DWORD,W.DWORD,W.DWORD,W.LPCWSTR],W.HANDLE),
            'MapViewOfFile':([W.HANDLE,W.DWORD,W.DWORD,W.DWORD,C.c_size_t],C.c_void_p),
            'WaitForSingleObject':([W.HANDLE,W.DWORD],W.DWORD),
            'ReleaseMutex':([W.HANDLE],W.BOOL),'CloseHandle':([W.HANDLE],W.BOOL),
            'UnmapViewOfFile':([C.c_void_p],W.BOOL),'GetTickCount':([],W.DWORD)}
        for name,(args,result) in specs.items():
            fn=getattr(self.api,name);fn.argtypes=args;fn.restype=result
        self.owner=self.api.CreateMutexW(None,True,f'Local\\CrominUI_{pid}_Controller')
        owner_error=C.get_last_error()
        if not self.owner:raise C.WinError(owner_error)
        if owner_error==183:
            self.api.CloseHandle(self.owner);self.owner=None
            raise RuntimeError('Ja existe um controlador Cromin para este PID')
        self.mutex=self.api.CreateMutexW(None,False,f'Local\\CrominUI_{pid}_Mutex')
        self.mapping=self.api.CreateFileMappingW(W.HANDLE(-1),None,4,0,C.sizeof(State),f'Local\\CrominUI_{pid}')
        self.address=self.api.MapViewOfFile(self.mapping,0xF001F,0,0,C.sizeof(State)) if self.mapping else None
        if not self.address:raise C.WinError(C.get_last_error())
        self.state=State.from_address(self.address)
        with self.lock():
            if self.state.magic==MAGIC and self.state.uiReady and not self.state.uiClosed:
                raise RuntimeError('O painel anterior ainda esta aberto. Feche-o antes de reiniciar o controlador')
            C.memset(self.address,0,C.sizeof(State));self.state.magic=MAGIC;self.state.version=1;self.state.bytes=C.sizeof(State)
            self.state.hpThreshold=80;self.state.healCooldownMs=2000;self.state.targetName='Rat'
            self.state.heartbeat=self.api.GetTickCount();self.state.status='Conectando ao Antica'
    @contextlib.contextmanager
    def lock(self):
        result=self.api.WaitForSingleObject(self.mutex,1000)
        if result not in (0,0x80):raise RuntimeError('Mutex da interface ocupado')
        try:yield self.state
        finally:self.api.ReleaseMutex(self.mutex)
    def snapshot(self):
        with self.lock() as s:
            s.heartbeat=self.api.GetTickCount()
            return State.from_buffer_copy(s)
    def status(self,text,detail=None):
        with self.lock() as s:
            s.heartbeat=self.api.GetTickCount();s.status=str(text)[:255]
            if detail is not None:s.detail=str(detail)[:255]
    def disable(self,bits=15):
        with self.lock() as s:s.flags&=~bits
    def close(self):
        if getattr(self,'address',None):
            self.disable();self.state=None;self.api.UnmapViewOfFile(self.address);self.address=None
        for name in ('mapping','mutex','owner'):
            handle=getattr(self,name,None)
            if handle:
                if name=='owner':self.api.ReleaseMutex(handle)
                self.api.CloseHandle(handle);setattr(self,name,None)
