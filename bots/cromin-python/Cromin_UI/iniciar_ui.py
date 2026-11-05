"""Carrega o painel dentro do Antica e mantem o controlador em execucao."""
import argparse
import ctypes as C
from pathlib import Path
import signal
import struct
import subprocess
import sys
import time
from value_scan import Memory,choose_pid
from bag_read import verify_build
from ui_ipc import Bridge
from controller import Controller


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid',type=int);parser.add_argument('--thread',type=int)
    args=parser.parse_args();folder=Path(__file__).resolve().parent
    if C.sizeof(C.c_void_p)!=8:raise ValueError('Use Python de 64 bits')
    pid=choose_pid('Antica Retro opengl.exe',args.pid);memory=Memory(pid);bridge=None;controller=None;stopped=False
    def stop(signum,frame):
        nonlocal stopped
        stopped=True
        if bridge:bridge.disable()
        if controller:controller.quit=True
        print('[PARADA] Aguardando a operacao atual. Feche o painel no jogo antes de reiniciar.')
    old_signal=signal.signal(signal.SIGINT,stop)
    try:
        base,path=verify_build(memory)
        thread=struct.unpack('<I',memory.exact(base+0xE53E18,4))[0]
        if not thread or (args.thread and args.thread!=thread):raise ValueError('Thread do ciclo do jogo diverge da informada')
        bridge=Bridge(pid)
        controller=Controller(memory,base,pid,thread,bridge,folder)
        if stopped:return 0
        print(f'[CLIENTE] PID={pid}; thread do jogo={thread}; build conferida.')
        print('[UI] Carregando painel. Todas as automacoes iniciam desligadas.')
        command=[str(folder/'Cromin_UI_Load.exe'),'--pid',str(pid),str(folder/'Cromin_UI.dll')]
        # Aguarda LoadLibrary retornar; nao mata o carregador no meio da operacao.
        child=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,errors='replace')
        for line in child.stdout:print(line,end='')
        if child.wait()!=0:raise RuntimeError('Nao foi possivel carregar a DLL da interface')
        if stopped:return 0
        deadline=time.monotonic()+15
        while time.monotonic()<deadline:
            if bridge.snapshot().uiReady:break
            time.sleep(0.1)
        else:raise RuntimeError('DLL carregada, mas painel nao confirmou inicializacao em 15 segundos')
        print('[PRONTO] Painel dentro do Antica. Mantenha este controlador aberto.')
        print(f'[RELATORIOS] {folder}')
        controller.run();return 0
    finally:
        if bridge:bridge.close()
        memory.close();signal.signal(signal.SIGINT,old_signal)


if __name__=='__main__':
    try:raise SystemExit(main())
    except Exception as error:
        print(f'[ERRO] {error}',file=sys.stderr);raise SystemExit(1)
