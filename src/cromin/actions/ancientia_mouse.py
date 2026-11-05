"""Clique direito normal em coordenadas calibradas do mapa, sem hotkey."""
import ctypes as C
from ctypes import wintypes as W
import time
from cromin.actions.ancientia_keyboard import Keyboard


def project(calibration,player,monster,rect):
    if list(rect)!=calibration['rect']:
        raise ValueError('Tamanho da janela mudou; calibre o mapa novamente.')
    if player[2]!=monster[2]:raise ValueError('Monstro em outro andar')
    dx,dy=monster[0]-player[0],monster[1]-player[1]
    if abs(dx)>4 or abs(dy)>3 or (dx==0 and dy==0):
        raise ValueError('Monstro fora da area calibrada ou no SQM do jogador')
    tile=calibration['tile'];x,y=calibration['center']
    if not 8<=tile<=128:raise ValueError('Tamanho do SQM invalido')
    point=(round(x+dx*tile),round(y+dy*tile))
    if not rect[0]<=point[0]<rect[2] or not rect[1]<=point[1]<rect[3]:
        raise ValueError('Clique projetado fora da janela')
    return point


class Mouse(Keyboard):
    def __init__(self,pid):
        super().__init__(pid)
        api=self.api
        api.GetCursorPos.argtypes=[C.POINTER(W.POINT)];api.GetCursorPos.restype=W.BOOL
        api.SetCursorPos.argtypes=[C.c_int,C.c_int];api.SetCursorPos.restype=W.BOOL
        api.ScreenToClient.argtypes=[W.HWND,C.POINTER(W.POINT)];api.ScreenToClient.restype=W.BOOL
        api.ClientToScreen.argtypes=[W.HWND,C.POINTER(W.POINT)];api.ClientToScreen.restype=W.BOOL
        api.GetClientRect.argtypes=[W.HWND,C.POINTER(W.RECT)];api.GetClientRect.restype=W.BOOL
        api.WindowFromPoint.argtypes=[W.POINT];api.WindowFromPoint.restype=W.HWND
        api.GetAncestor.argtypes=[W.HWND,W.UINT];api.GetAncestor.restype=W.HWND

    def cursor(self):
        point=W.POINT()
        if not self.api.GetCursorPos(C.byref(point)):raise RuntimeError('Nao leu posicao do mouse')
        return point

    def geometry(self):
        if not self.focused():raise ValueError('Volte a janela do Ancientia para calibrar/atacar.')
        hwnd=self.api.GetForegroundWindow();rect=W.RECT()
        if not self.api.GetClientRect(hwnd,C.byref(rect)):raise RuntimeError('Nao leu tamanho da janela')
        return hwnd,[rect.left,rect.top,rect.right,rect.bottom]

    def capture_point(self):
        hwnd,rect=self.geometry();point=self.cursor()
        if not self.api.ScreenToClient(hwnd,C.byref(point)):raise RuntimeError('Falha convertendo coordenada')
        if not rect[0]<=point.x<rect[2] or not rect[1]<=point.y<rect[3]:
            raise ValueError('Mouse fora da area do cliente')
        return dict(hwnd=int(hwnd),rect=rect,point=[point.x,point.y])

    def right_click(self,calibration,player,monster):
        hwnd,rect=self.geometry()
        if int(hwnd)!=calibration['hwnd']:raise ValueError('Janela mudou; calibre novamente.')
        x,y=project(calibration,player,monster,rect)
        if self.stop_pressed() or any(self.api.GetAsyncKeyState(k)&0x8000 for k in (1,2,0x10,0x11,0x12,0x5B,0x5C)):
            return False
        point=W.POINT(x,y)
        if not self.api.ClientToScreen(hwnd,C.byref(point)):raise RuntimeError('Falha convertendo clique')
        under=self.api.GetAncestor(self.api.WindowFromPoint(point),2)
        if under!=hwnd:raise ValueError('Outra janela cobre o ponto do monstro; clique cancelado.')
        previous=self.cursor()
        if not self.api.SetCursorPos(point.x,point.y):raise RuntimeError('Nao posicionou mouse')
        down=self.Input();down.type=0;down.data.mi.flags=0x0008
        up=self.Input();up.type=0;up.data.mi.flags=0x0010
        try:
            if not self.focused() or self.stop_pressed():return False
            if self.api.SendInput(1,C.byref(down),C.sizeof(down))!=1:
                raise RuntimeError('Windows recusou o clique')
            try:time.sleep(0.03)
            finally:
                if self.api.SendInput(1,C.byref(up),C.sizeof(up))!=1:
                    raise RuntimeError('Soltura do botao direito nao confirmada')
        finally:
            current=self.cursor()
            if (current.x,current.y)==(point.x,point.y):self.api.SetCursorPos(previous.x,previous.y)
        return True
