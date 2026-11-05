"""Observe manual Look calls; detach before any native action helper starts."""
import ctypes as C
from ctypes import wintypes as W
import json,os,subprocess,threading,uuid
from cromin.core.paths import BIN

class StopEvent:
    def __init__(self):
        self.name='Local\\CrominLook_'+uuid.uuid4().hex
        self.api=C.WinDLL('kernel32',use_last_error=True)
        self.api.CreateEventW.argtypes=[C.c_void_p,W.BOOL,W.BOOL,W.LPCWSTR];self.api.CreateEventW.restype=W.HANDLE
        self.api.SetEvent.argtypes=[W.HANDLE];self.api.SetEvent.restype=W.BOOL
        self.api.CloseHandle.argtypes=[W.HANDLE];self.api.CloseHandle.restype=W.BOOL
        self.handle=self.api.CreateEventW(None,True,False,self.name)
        if not self.handle:raise C.WinError(C.get_last_error())
    def set(self):
        if self.handle and not self.api.SetEvent(self.handle):raise C.WinError(C.get_last_error())
    def close(self):
        if self.handle:self.api.CloseHandle(self.handle);self.handle=None

class LookObserver:
    def __init__(self,pid,bridge,record,event_factory=StopEvent,launcher=subprocess.Popen):
        self.pid,self.bridge,self.record=pid,bridge,record
        self.event_factory,self.launcher=event_factory,launcher
        self.process=None;self.cancel=None;self.reader=None;self.done=threading.Event()
        self.ready=False;self.closed_clean=None;self.returncode=None;self.error=None
        self.disabled=False;self.critical=None
    def status(self,message):
        with self.bridge.lock() as s:s.lookStatus=message
    def message(self,data):
        if not isinstance(data,dict):return
        event=data.get('event')
        if event=='ready':self.ready=True;self.status('Look ativo: use o Look normal no jogo')
        elif event=='closed':
            self.closed_clean=data.get('clean') is True and data.get('detached') is True
            if data.get('error'):self.error=str(data['error'])[:255]
        elif event=='look':
            kind=data.get('kind');value=data.get('id')
            if kind not in ('item','monster','unknown') or type(value) is not int:return
            if kind=='item' and not 1<=value<=65535:return
            if kind=='monster' and not 0<=value<=0xffffffff:return
            if kind=='unknown' and value!=0:return
            with self.bridge.lock() as s:
                s.lastLookKind=kind;s.lastLookId=value if kind=='item' else 0
                s.lastLookObject=str(data.get('object',''))
                s.lookStatus=(f'Ultimo Look: item ID {value}' if kind=='item' else 'Look em criatura ou tipo sem ID de item validado')
            self.record('manual_look',data)
    def consume(self):
        try:
            for line in self.process.stdout:
                line=line.strip()
                if len(line)>8192:continue
                try:data=json.loads(line)
                except (ValueError,TypeError):self.error=line[:255];continue
                try:self.message(data)
                except Exception as error:
                    self.error=str(error)
                    if self.cancel:self.cancel.set()
            self.returncode=self.process.wait()
        except Exception as error:
            self.error=str(error)
            if self.cancel:self.cancel.set()
            try:self.returncode=self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:self.critical='Observador Look nao encerrou; nenhuma acao nativa pode iniciar'
        finally:self.done.set()
    def start(self):
        if self.disabled or self.critical:return
        if self.process is not None:
            if not self.done.is_set():return
            self.pause()
            if self.disabled:return
        self.ready=False;self.closed_clean=None;self.returncode=None;self.error=None;self.done.clear()
        try:
            self.cancel=self.event_factory()
            self.process=self.launcher([str(BIN/'OTC_Look_Watch.exe'),'--pid',str(self.pid),'--parent',str(os.getpid()),'--stop-event',self.cancel.name],
                stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace',
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        except OSError as error:
            if self.cancel:self.cancel.close();self.cancel=None
            self.process=None;self.disabled=True;self.status('Look indisponivel: '+str(error));self.record('look_unavailable',{'error':str(error)});return
        self.status('Iniciando observacao de Look...')
        self.reader=threading.Thread(target=self.consume,name='CrominLookReader',daemon=True);self.reader.start()
    def pause(self):
        if self.critical:raise RuntimeError(self.critical)
        if self.process is None:return
        self.cancel.set()
        if not self.done.wait(10):
            self.critical='Look ainda nao confirmou detach; acao nativa nao iniciada'
            raise RuntimeError(self.critical)
        code=self.process.poll()
        if code is None:
            self.critical='Observador Look ainda ativo; acao nativa nao iniciada';raise RuntimeError(self.critical)
        safe=code==0 and self.closed_clean is True
        preflight_failed=code==1 and not self.ready and self.closed_clean is None
        self.process.stdout.close();self.process=None;self.reader=None
        self.cancel.close();self.cancel=None
        if not safe:
            self.disabled=True
            if not preflight_failed:
                self.critical='Look nao confirmou restauracao/detach; acao nativa nao iniciada'+(': '+self.error if self.error else '')
                self.status(self.critical);raise RuntimeError(self.critical)
            self.status('Look indisponivel: '+(self.error or 'preflight recusado'))
            self.record('look_unavailable',{'error':self.error,'exit_code':code})
        else:self.status('Look pausado durante acao nativa')
    def close(self):self.pause()
