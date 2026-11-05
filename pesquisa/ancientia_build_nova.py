"""Verificacao somente leitura da build AncientiaDX atualizada em 05/11."""
import argparse
from datetime import datetime,timezone
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
import psutil
from cromin.readers.memory import Memory
from cromin.readers.build import parse_pe
from cromin.readers.ancientia_player import resolve_rtti
from cromin.readers.ancientia_pending import decode_name

NEW=dict(sha256='e6c7daedcab8c324697ce55c564a7300997507fd44454854109707c344208be2',
         machine=0x14c,timestamp=0x6AEBB27C,size_image=0xDDF000,
         gameRva=0xD43090,playerGlobalRva=0xD430C0,playerVtableRva=0x85D974,monsterVtableRva=0xABA974)
GETTERS={
    'localPlayer':(0x58CE0+0x2D,'8b4930'),
    'health':(0x5DBE0,'dd8188050000c3'),
    'maxHealth':(0x5DBF0,'dd8190050000c3'),
    'mana':(0x5DC50,'dd81c0050000c3'),
    'level':(0x5DC30,'dd81b0050000c3'),
    'experience':(0x5DC20,'dd81a8050000c3'),
    'maxManaPrefix':(0x5DC60,'f20f1081c8050000')}
OFFSETS=dict(health=0x588,maxHealth=0x590,mana=0x5C0,experience=0x5A8,level=0x5B0)


def inspect_pid(pid,factory=Memory):
    memory=factory(pid)
    try:
        path=memory.executable()
        if Path(path).name.casefold()!='ancientiadx.exe':raise ValueError(f'Este PID pertence a {Path(path).name}, nao ao AncientiaDX.exe')
        base,size=memory.main_module(path)
        def read(rva,n):
            if not 0<=rva<=size-n:raise ValueError('Leitura fora do modulo')
            return memory.exact(base+rva,n)
        disk=Path(path).read_bytes();sha=hashlib.sha256(disk).hexdigest()
        if sha!=NEW['sha256']:raise ValueError('O executavel mudou de novo ou nao corresponde ao inventario enviado')
        pe=parse_pe(read)
        for key in ('machine','timestamp','size_image'):
            if pe[key]!=NEW[key]:raise ValueError('Cabecalho de memoria divergente: '+key)
        for name,(rva,expected) in GETTERS.items():
            if read(rva,len(bytes.fromhex(expected)))!=bytes.fromhex(expected):
                raise ValueError('Getter em memoria divergente: '+name)
        rtti=resolve_rtti(disk,'LocalPlayer')
        if not any(int(r['vtableRva'],16)==NEW['playerVtableRva'] and r['completeObjectOffset']==0 for r in rtti):
            raise ValueError('RTTI LocalPlayer nao confere')
        player=struct.unpack('<I',read(NEW['playerGlobalRva'],4))[0]
        if not 0x10000<=player<=0xFFFFFFFF-0x5D0:raise ValueError('Jogador indisponivel; entre no personagem')
        blob=memory.exact(player,0x5D0)
        if struct.unpack_from('<I',blob)[0]!=base+NEW['playerVtableRva']:raise ValueError('Vtable do jogador em memoria nao confere')
        name=decode_name(memory,blob,0x44)
        if not name:raise ValueError('Nome do jogador indisponivel')
        fields={name:struct.unpack_from('<d',blob,off)[0] for name,off in OFFSETS.items()}
        if any(not math.isfinite(v) or v<0 for v in fields.values()) or fields['level']<1 or fields['maxHealth']<1 or fields['health']>fields['maxHealth']:
            raise ValueError('Atributos do jogador inconsistentes')
        xyz=struct.unpack_from('<III',blob,0xC)
        if not (0<xyz[0]<65535 and 0<xyz[1]<65535 and 0<=xyz[2]<=15):raise ValueError('Posicao inconsistente')
        if struct.unpack('<I',read(NEW['playerGlobalRva'],4))[0]!=player:raise ValueError('Jogador mudou durante a leitura')
        return dict(pid=pid,status='new_build_read_verified',executable=path,sha256=sha,moduleBase=hex(base),
                    playerAddress=hex(player),playerGlobalRva=hex(NEW['playerGlobalRva']),
                    name=name,position=dict(zip(('x','y','z'),xyz)),fields=fields,
                    maxManaRawCandidate=struct.unpack_from('<d',blob,0x5C8)[0],
                    lightRawOnly=dict(color=blob[0xC8],level=blob[0xC9]),
                    actionsEnabled=False,lightWriteValidated=False,nativeAbiValidated=False)
    finally:memory.close()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pids',nargs='+',type=int,required=True)
    args=parser.parse_args()
    if sys.platform!='win32' or any(pid<=0 for pid in args.pids):parser.error('Use Windows e PIDs positivos')
    result=dict(schema='ancientia-new-build-verification-v1',createdUtc=datetime.now(timezone.utc).isoformat(),
                expectedSha256=NEW['sha256'],memoryWritten=False,debuggerAttached=False,results=[])
    for pid in dict.fromkeys(args.pids):
        try:
            entry=inspect_pid(pid);f=entry['fields']
            print(f"PID {pid}: {entry['name']} | HP {f['health']:g}/{f['maxHealth']:g} | Mana {f['mana']:g} | Nivel {f['level']:g} | XP {f['experience']:g} | {entry['position']}")
        except (OSError,RuntimeError,ValueError,psutil.Error) as error:
            entry=dict(pid=pid,status='not_verified',error=str(error));print(f'PID {pid}: {error}')
        result['results'].append(entry)
    output=Path(__file__).resolve().parent/'reports'/('Ancientia_build_nova_'+datetime.now().strftime('%Y%m%d_%H%M%S_%f')+'.json')
    output.parent.mkdir(exist_ok=True);output.write_text(json.dumps(result,indent=2,ensure_ascii=False),encoding='utf-8')
    print('Relatorio:',output)
    print('Nenhuma acao foi liberada. Confira os valores com o jogo e envie o JSON.')
    return 0 if any(r['status']=='new_build_read_verified' for r in result['results']) else 2


if __name__=='__main__':raise SystemExit(main())
