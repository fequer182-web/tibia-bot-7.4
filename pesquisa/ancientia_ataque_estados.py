"""Descoberta de ataque OFF/ON/OFF/ON: somente leitura, sem depurador."""
import argparse
from datetime import datetime,timezone
import json
from pathlib import Path
import struct
import sys
import ctypes as C
from ctypes import wintypes as W
import time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
import psutil
from cromin.readers.memory import Memory,blocks
from cromin.readers.ancientia import DX
from cromin.readers.ancientia_pending import connect,snapshot
from cromin.readers.build import parse_pe
from cromin.readers.ancientia_actions import read_monster,GAME_RVA,MONSTER_VTABLE_RVA

PHASES=[('off','PARE de atacar. Cancele o alvo normalmente no jogo.'),
        ('on','ATAQUE um monstro e mantenha o combate durante a captura.'),
        ('off','PARE de atacar novamente e cancele o alvo.'),
        ('on','ATAQUE um monstro novamente. Pode ser outro monstro.')]


def wait_key(focused,down,pause=time.sleep):
    # Aguarda uma nova tecla e sua soltura, sempre com o jogo em primeiro plano.
    while True:
        if focused() and down():
            while down():pause(.02)
            if focused():return
        pause(.02)


class CaptureKey:
    def __init__(self,pid,key):
        self.pid=pid;self.key=key;self.api=C.WinDLL('user32',use_last_error=True)
        self.api.GetForegroundWindow.argtypes=[];self.api.GetForegroundWindow.restype=W.HWND
        self.api.GetWindowThreadProcessId.argtypes=[W.HWND,C.POINTER(W.DWORD)];self.api.GetWindowThreadProcessId.restype=W.DWORD
        self.api.GetAsyncKeyState.argtypes=[C.c_int];self.api.GetAsyncKeyState.restype=C.c_short
    def focused(self):
        pid=W.DWORD();self.api.GetWindowThreadProcessId(self.api.GetForegroundWindow(),C.byref(pid))
        return pid.value==self.pid
    def wait(self):wait_key(self.focused,lambda:bool(self.api.GetAsyncKeyState(self.key)&0x8000))


def byte_transitions(states):
    if len(states)!=4:return []
    result=[]
    for offset,a in states[0].items():
        b=states[1].get(offset);c=states[2].get(offset);d=states[3].get(offset)
        if None not in (a,b,c,d) and a==c and b==d and a!=b:
            result.append(dict(gameOffset=hex(offset),off=a,on=b,status='repeated_byte_state_only'))
    return result


def target_snapshot(memory,base,words):
    address=words.get(base+GAME_RVA+0x34)
    if address is None:return dict(status='reference_unstable_or_unreadable')
    if address==0:return dict(address='0x0',status='null')
    try:
        raw=memory.exact(address,0x80)
        return dict(address=hex(address),vtable=hex(struct.unpack_from('<I',raw)[0]),
                    monsterVtableMatch=struct.unpack_from('<I',raw)[0]==base+MONSTER_VTABLE_RVA,
                    idAt40=struct.unpack_from('<I',raw,0x40)[0],hpPercentAt5C=raw[0x5c],
                    monster=read_monster(memory,base,address))
    except (OSError,RuntimeError) as error:return dict(address=hex(address),error=str(error))


def stable_region(memory,address,length):
    stats=dict(read_bytes=0,failed_bytes=0,failed_pages=0,failure_examples=[])
    first={};second={}
    for output in (first,second):
        for start,data in blocks(memory,address,length,stats):
            if data is None:continue
            for offset in range((-start)%4,len(data)-3,4):
                output[start+offset]=struct.unpack_from('<I',data,offset)[0]
    stable={key:value for key,value in first.items() if second.get(key)==value}
    return stable,dict(stats,stableWordCount=len(stable),firstWordCount=len(first),secondWordCount=len(second))


def find_on(memory,base,off,on):
    candidates={}
    for address,value in on.items():
        if address not in off or off[address]==value or not 0x10000<=value<=0xFFFFFFFF-0x80:continue
        try:monster=read_monster(memory,base,value)
        except (OSError,RuntimeError):continue
        if monster:
            try:raw_id=memory.exact(value+0x40,4)
            except (OSError,RuntimeError):continue
            monster['id']=struct.unpack('<I',raw_id)[0]
            candidates[address]=dict(globalRva=hex(address-base),baseline=off[address],monster=monster)
    return candidates


def intersect_transitions(first_on,second_off,second_on):
    result=[]
    for address,first in first_on.items():
        if second_off.get(address)!=first.get('baseline',0) or address not in second_on:continue
        second=second_on[address]
        result.append(dict(globalRva=first['globalRva'],firstMonster=first['monster'],
                           secondMonster=second['monster'],offValue=hex(first.get('baseline',0)),pattern='baseline/Monster/baseline/Monster',
                           status='repeated_state_transition',nativeCallAbiValidated=False))
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid',required=True,type=int)
    parser.add_argument('--captura',choices=['F9','F10','F11','F12'],default='F12',help='Tecla livre no jogo para capturar, sem trocar de janela')
    parser.add_argument('--console',action='store_true',help='Alternativa antiga: Enter na console')
    parser.add_argument('--personagem',default='Personagem A',help='Confere o personagem escolhido antes da coleta')
    args=parser.parse_args()
    if sys.platform!='win32' or args.pid<=0:parser.error('Use Windows e PID positivo.')
    root=Path(__file__).resolve().parent
    output=root/'reports'/('Ancientia_ataque_estados_'+datetime.now().strftime('%Y%m%d_%H%M%S_%f')+'.json')
    result=dict(schema='ancientia-attack-state-transitions-v2',pid=args.pid,sha256=DX['sha256'],
                debuggerAttached=False,memoryWritten=False,nativeCallsRequested=False,
                samples=[],candidates=[],gameByteCandidates=[],captureMode='console' if args.console else args.captura,status='incomplete')
    memory=None
    def save():
        output.parent.mkdir(exist_ok=True)
        output.write_text(json.dumps(result,indent=2,ensure_ascii=False),encoding='utf-8')
    try:
        memory=Memory(args.pid);base=connect(memory)
        selected,_=snapshot(memory,base)
        if args.personagem and (selected['names']['0x44'] or '').casefold()!=args.personagem.casefold():
            raise ValueError(f"O PID esta com {selected['names']['0x44']}; o personagem escolhido e {args.personagem}")
        path=memory.executable();_,size=memory.main_module(path)
        pe=parse_pe(lambda off,n: memory.exact(base+off,n) if 0<=off<=size-n else b'')
        sections=[s for s in pe['sections'] if s['name']=='.data']
        if len(sections)!=1:raise ValueError('Secao .data nao e unica')
        section=sections[0];length=section['virtual'] or section['raw']
        if not 0<length<=16*1024*1024:raise ValueError('Tamanho .data fora dos limites')
        result.update(moduleBase=hex(base),processCreateTime=psutil.Process(args.pid).create_time(),
                      section=dict(rva=hex(section['rva']),length=length))
        states=[];game_bytes=[];first_on={};second_off={};identity=None
        trigger=None if args.console else CaptureKey(args.pid,0x6f+int(args.captura[1:]))
        print('Feche o painel Cromin. Coleta sem cronometro: parado -> atacando -> parado -> atacando.')
        print('Use Enter na console.' if args.console else f'Fique na janela do jogo e pressione {args.captura} em cada estado. Use uma tecla livre nas configuracoes do cliente.')
        for index,(phase,instruction) in enumerate(PHASES):
            print(f'\nETAPA {index+1}/4: {instruction}')
            if args.console:
                if input('Enter para capturar este estado: ').strip().casefold() in ('sair','q'):
                    result['status']='cancelled';return 130
            else:
                print(f'No jogo: {args.captura} captura. Aguarde o sinal antes de mudar para o proximo estado.',flush=True)
                trigger.wait()
            current,_=snapshot(memory,base)
            group=(current['objectAddress'],tuple(current['names'].values()))
            if identity is not None and group!=identity:raise ValueError('Jogador mudou entre estados; coleta encerrada sem combinar personagens')
            identity=group
            words,coverage=stable_region(memory,base+section['rva'],length)
            first=memory.exact(base+GAME_RVA,0x800);second=memory.exact(base+GAME_RVA,0x800)
            game_bytes.append({i:a for i,(a,b) in enumerate(zip(first,second)) if a==b})
            recheck,_=snapshot(memory,base)
            if recheck['objectAddress']!=current['objectAddress'] or recheck['names']!=current['names']:
                raise ValueError('Jogador mudou durante captura')
            if trigger and not trigger.focused():raise ValueError('O jogo perdeu o foco durante a captura; nao combinar este estado')
            states.append(words)
            sample=dict(stage=index+1,declaredState=phase,utc=datetime.now(timezone.utc).isoformat(),
                        player=current['objectAddress'],names=current['names'],position=current['positions']['0xc'],
                        coverage=coverage,attackReference=target_snapshot(memory,base,words),gameWords={hex(offset):hex(words[base+GAME_RVA+offset])
                                                   for offset in range(0,0x100,4) if base+GAME_RVA+offset in words})
            if index==1:
                first_on=find_on(memory,base,states[0],words)
                sample['newMonsterReferences']=list(first_on.values())
                print('Referencias candidatas apos iniciar ataque:',len(first_on))
            elif index==2:second_off=words
            elif index==3:
                second_on=find_on(memory,base,second_off,words)
                result['candidates']=intersect_transitions(first_on,second_off,second_on)
                result['gameByteCandidates']=byte_transitions(game_bytes)
                result['status']='repeated_candidates' if result['candidates'] or result['gameByteCandidates'] else 'no_repeated_candidates'
            result['samples'].append(sample);save()
            print('Estado capturado. Pode preparar a proxima etapa sem pressa.')
            if trigger:
                import winsound
                try:winsound.Beep(900+index*150,180)
                except RuntimeError:pass
        print('\nReferencias que acompanharam as duas transicoes:',len(result['candidates']))
        print('Isso identifica candidatos de estado; nao valida a ABI de chamadas nativas.')
        return 0
    except (OSError,ValueError,RuntimeError,psutil.Error) as error:
        result['error']=str(error);result['status']='error';print('[ERRO]',error,file=sys.stderr);return 1
    except (KeyboardInterrupt,EOFError):
        result['status']='cancelled';print('\nColeta encerrada.');return 130
    finally:
        if memory:memory.close()
        save();print('Relatorio:',output)


if __name__=='__main__':raise SystemExit(main())
