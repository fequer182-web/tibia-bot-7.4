from cromin.core.paths import DATA, BIN
"""Conexao em segundo plano; nao carrega DLL no cliente."""
import ctypes as C
from ctypes import wintypes as W
from pathlib import Path
import struct
import threading
from cromin.readers.bag import verify_build
from cromin.readers.memory import Memory,choose_pid
from cromin.core.controller import Controller
from cromin.core.state import Bridge

class Owner:
    def __init__(self,pid):
        self.api=C.WinDLL('kernel32',use_last_error=True)
        self.api.CreateMutexW.argtypes=[C.c_void_p,W.BOOL,W.LPCWSTR];self.api.CreateMutexW.restype=W.HANDLE
        self.api.ReleaseMutex.argtypes=[W.HANDLE];self.api.ReleaseMutex.restype=W.BOOL
        self.api.CloseHandle.argtypes=[W.HANDLE];self.api.CloseHandle.restype=W.BOOL
        self.handle=self.api.CreateMutexW(None,True,f'Local\\CrominExternalController_{pid}')
        error=C.get_last_error()
        if not self.handle:raise C.WinError(error)
        if error==183:
            self.api.CloseHandle(self.handle);self.handle=None
            raise RuntimeError('Ja existe um painel externo conectado a este PID')
    def close(self):
        if self.handle:
            self.api.ReleaseMutex(self.handle);self.api.CloseHandle(self.handle);self.handle=None

class Session:
    def __init__(self,pid=None,thread=None,folder=None):
        self.pid_requested,self.thread_requested=pid,thread
        self.pid=self.thread_id=None;self.bridge=Bridge();self.controller=None;self.connected=False
        self.stopping=False;self.error=None;self.folder=Path(folder or DATA)
        self.worker=None
    def start(self):
        if self.worker and self.worker.is_alive():return
        self.worker=threading.Thread(target=self._run,name='CrominExternalWorker',daemon=False);self.worker.start()
    def _run(self):
        memory=None;owner=None
        try:
            if C.sizeof(C.c_void_p)!=8:raise RuntimeError('Use Python de 64 bits')
            self.bridge.status('Conferindo processo e build...','Aguarde a conexao; nenhuma acao sera iniciada automaticamente.')
            pid=choose_pid('Antica Retro opengl.exe',self.pid_requested);owner=Owner(pid);memory=Memory(pid)
            base,path=verify_build(memory)
            thread=struct.unpack('<I',memory.exact(base+0xE53E18,4))[0]
            if not thread or (self.thread_requested and self.thread_requested!=thread):raise ValueError('Thread do jogo diferente da informada')
            if self.stopping:return
            controller=Controller(memory,base,pid,thread,self.bridge,self.folder)
            enable_look=getattr(controller,'enable_look',None)
            if enable_look:enable_look()
            self.controller=controller;self.pid=pid;self.thread_id=thread;self.connected=True
            self.bridge.status('Conectado. Automacoes desligadas.',f'PID {pid} | thread do jogo {thread}')
            if self.stopping:controller.quit=True
            controller.run()
        except Exception as error:
            self.error=str(error);self.bridge.status('Falha na conexao ou no controlador',self.error)
        finally:
            self.connected=False;self.bridge.disable()
            if self.controller:
                try:
                    close=getattr(self.controller,'close',None)
                    if close:close()
                except Exception as error:
                    self.error=str(error);self.bridge.status('Erro ao encerrar observador ou restaurar luz',self.error)
            if memory:memory.close()
            if owner:owner.close()
            if not self.error:self.bridge.status('Desconectado','Nenhuma nova acao sera enviada.')
    def stop(self):
        self.stopping=True;self.bridge.disable()
        if self.controller:self.controller.quit=True
        self.bridge.status('Aguardando a operacao atual terminar...','Uma chamada ja iniciada sera restaurada antes de encerrar.')
    def alive(self):return bool(self.worker and self.worker.is_alive())
