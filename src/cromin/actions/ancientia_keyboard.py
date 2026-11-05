"""Teclas normais do Windows apenas quando o PID alvo esta em primeiro plano."""
import ctypes as C
from ctypes import wintypes as W
import time
from cromin.core.ancientia_runner import KEYS


class Keyboard:
    def __init__(self,pid):
        self.pid=pid
        self.api=C.WinDLL('user32',use_last_error=True)
        self.api.GetForegroundWindow.argtypes=[];self.api.GetForegroundWindow.restype=W.HWND
        self.api.GetWindowThreadProcessId.argtypes=[W.HWND,C.POINTER(W.DWORD)]
        self.api.GetWindowThreadProcessId.restype=W.DWORD
        self.api.GetAsyncKeyState.argtypes=[C.c_int];self.api.GetAsyncKeyState.restype=C.c_short
        class Mouse(C.Structure):
            _fields_=[('dx',W.LONG),('dy',W.LONG),('data',W.DWORD),('flags',W.DWORD),('time',W.DWORD),('extra',C.c_size_t)]
        class Key(C.Structure):
            _fields_=[('vk',W.WORD),('scan',W.WORD),('flags',W.DWORD),('time',W.DWORD),('extra',C.c_size_t)]
        class Payload(C.Union):_fields_=[('mi',Mouse),('ki',Key)]
        class Input(C.Structure):_fields_=[('type',W.DWORD),('data',Payload)]
        self.Input,self.Key=Input,Key
        self.api.SendInput.argtypes=[W.UINT,C.POINTER(Input),C.c_int];self.api.SendInput.restype=W.UINT
        if C.sizeof(Input)!=40:raise RuntimeError('Use Python Windows 64 bits.')

    def focused(self):
        pid=W.DWORD()
        self.api.GetWindowThreadProcessId(self.api.GetForegroundWindow(),C.byref(pid))
        return pid.value==self.pid

    def stop_pressed(self):return bool(self.api.GetAsyncKeyState(0x77)&0x8000)  # F8

    def send(self,name):
        vk=KEYS[name]
        if not self.focused() or self.stop_pressed():return False
        # Nao compor atalhos com modificadores ou uma tecla mantida pelo usuario.
        if any(self.api.GetAsyncKeyState(k)&0x8000 for k in (0x10,0x11,0x12,0x5B,0x5C,vk)):return False
        extended=1 if name in ('LEFT','RIGHT','UP','DOWN') else 0
        down=self.Input();down.type=1;down.data.ki=self.Key(vk,0,extended,0,0)
        up=self.Input();up.type=1;up.data.ki=self.Key(vk,0,extended|2,0,0)
        if self.api.SendInput(1,C.byref(down),C.sizeof(down))!=1:
            raise RuntimeError('Windows recusou a tecla; nenhuma repeticao automatica.')
        try:
            time.sleep(0.04)
        finally:
            if self.api.SendInput(1,C.byref(up),C.sizeof(up))!=1:
                raise RuntimeError('Soltura da tecla nao confirmada; automacao interrompida.')
        return True
