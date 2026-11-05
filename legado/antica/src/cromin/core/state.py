"""Estado local entre a interface e o controlador. Nao abre memoria compartilhada no jogo."""
import contextlib
import copy
from dataclasses import dataclass,field
import threading

ATTACK,LOOT,CAVE,HEAL,LOOP,LIGHT,FOOD=1,2,4,8,16,32,64
ACTION_MASK=ATTACK|LOOT|CAVE|HEAL|LIGHT|FOOD

@dataclass(frozen=True)
class Position:
    x:int=0
    y:int=0
    z:int=0

@dataclass
class State:
    flags:int=0
    hpThreshold:int=80
    healCooldownMs:int=2000
    destination:int=0
    corpseIds:str='3994'
    lootIds:str='3031'
    dropIds:str=''
    dropScope:str='corpse'
    healText:str=''
    foodIds:str='3577'
    foodStatus:str='Auto Food desligado'
    lightLevel:int=31
    lightStatus:str='Desligada'
    lightObserved:int|None=None
    lookStatus:str='Look aguardando conexao'
    lastLookId:int=0
    lastLookKind:str=''
    lastLookObject:str=''
    targetName:str='Rat'
    targetAny:bool=False
    targetFollow:bool=True
    status:str='Desconectado'
    detail:str='Conecte ao Antica para iniciar o teste.'
    uiClosed:int=0
    waypointCount:int=0
    waypointIndex:int=0
    addSerial:int=0
    clearSerial:int=0
    removeSerial:int=0
    removeIndex:int=0
    waypoints:list=field(default_factory=lambda:[Position() for _ in range(64)])

class Bridge:
    def __init__(self):
        self.data=State();self.mutex=threading.RLock()
    @contextlib.contextmanager
    def lock(self):
        with self.mutex:yield self.data
    def snapshot(self):
        with self.lock() as s:return copy.deepcopy(s)
    def status(self,text,detail=None):
        with self.lock() as s:
            s.status=str(text)
            if detail is not None:s.detail=str(detail)
    def disable(self,bits=ACTION_MASK):
        with self.lock() as s:s.flags&=~bits
    def close(self):
        with self.lock() as s:s.flags&=~ACTION_MASK;s.uiClosed=1
