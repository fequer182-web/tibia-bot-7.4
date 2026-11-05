"""Controlador serial das chamadas ja validadas, usado pelo painel externo."""
from datetime import datetime
import json
import math
from pathlib import Path
import struct
import sys
import time
from ui_ipc import ATTACK,LOOT,CAVE,HEAL,LOOP,LIGHT,ACTION_MASK,Position
from light_control import ExternalLight
from cycle_common import run_child,native_completed,SkipCycle
from rat_cycle_once import cycle
from open_and_loot import open_and_collect
from world_read import WorldReader


class StopRequested(RuntimeError):pass


class Controller:
    def __init__(self,memory,base,pid,thread,bridge,folder=None,runner=run_child,clock=time.monotonic):
        self.memory,self.base,self.pid,self.thread,self.bridge=memory,base,pid,thread,bridge
        self.world=WorldReader(memory,base);self.folder=Path(folder or Path(__file__).resolve().parent)
        self.raw_runner,self.clock=runner,clock
        self.last_heal=-1e9;self.goal=None;self.handled=set();self.last_scan=0;self.native_busy=False
        self.add_serial=self.clear_serial=self.remove_serial=0
        self.phase='Pronto. Ative os modulos desejados.';self.fault=None;self.quit=False;self.last_settings=None
        self.settings=self.folder/'Cromin_Externo.settings.json'
        self.log=self.folder/('external_session_'+datetime.now().strftime('%Y%m%d_%H%M%S_%f')+'.jsonl')
        self.light=ExternalLight(memory,base,pid,self.record)
        self.load_settings()
    def record(self,event,data):
        with self.log.open('a',encoding='utf-8') as stream:
            stream.write(json.dumps({'time':datetime.now().isoformat(),'event':event,'data':data},ensure_ascii=False)+'\n')
    def load_settings(self):
        if not self.settings.is_file():return
        data=json.loads(self.settings.read_text(encoding='utf-8'))
        hp=int(data.get('hp',80));cooldown=int(data.get('cooldown',2000));destination=int(data.get('destination',0));spell=data.get('spell','')
        points=data.get('waypoints',[]);light=int(data.get('light',7))
        if not (1<=hp<=1000000 and 1000<=cooldown<=60000 and 0<=destination<=63 and isinstance(spell,str)
                and len(spell)<=15 and all(32<=ord(c)<=126 for c in spell) and len(points)<=64 and 0<=light<=31):
            raise ValueError('Configuracao salva invalida; revise Cromin_Externo.settings.json')
        with self.bridge.lock() as s:
            s.hpThreshold=hp;s.healCooldownMs=cooldown;s.destination=destination;s.healText=spell;s.lightLevel=light
            s.waypointCount=len(points);s.flags=LOOP if data.get('loop') else 0
            for index,p in enumerate(points):
                if not (0<p['x']<65535 and 0<p['y']<65535 and 0<=p['z']<=15):raise ValueError('Waypoint salvo invalido')
                s.waypoints[index]=Position(p['x'],p['y'],p['z'])
    def save_settings(self,s):
        data={'hp':s.hpThreshold,'cooldown':s.healCooldownMs,'destination':s.destination,'spell':s.healText,'light':s.lightLevel,
              'loop':bool(s.flags&LOOP),'waypoints':[{'x':p.x,'y':p.y,'z':p.z} for p in s.waypoints[:s.waypointCount]]}
        if data!=self.last_settings:
            temporary=self.settings.with_suffix('.tmp');temporary.write_text(json.dumps(data,indent=2),encoding='utf-8')
            temporary.replace(self.settings);self.last_settings=data
    def state(self):
        s=self.bridge.snapshot()
        if s.uiClosed or self.quit:raise StopRequested('Interface fechada ou parada solicitada')
        return s
    def active(self,bit):return bool(self.state().flags&bit)
    def health(self):
        pointer=self.world.u64(self.base+0xE52A48)
        if self.world.u64(pointer)!=self.base+0xAE99E0:raise ValueError('VTable do jogador mudou')
        hp=struct.unpack('<d',self.world.read(pointer+0x6C0,8))[0]
        if not math.isfinite(hp) or not 0<=hp<=1000000000:raise ValueError('HP atual invalido')
        return hp
    def run_native(self,command,prefix='[JSON] '):
        if self.native_busy:raise RuntimeError('Outra chamada nativa ja esta em andamento')
        self.state();self.native_busy=True
        try:
            result=self.raw_runner(command,prefix)
            self.record('native',result)
            return result
        finally:self.native_busy=False
    def require_native(self,result):
        if not native_completed(result):raise RuntimeError('Chamada/restauracao nao confirmada; automacoes interrompidas')
    def heal(self,s):
        hp=self.health()
        if not (s.flags&HEAL and hp>0 and hp<=s.hpThreshold and self.clock()-self.last_heal>=s.healCooldownMs/1000):return False
        spell=s.healText
        if not spell or len(spell)>15 or any(not 32<=ord(c)<=126 for c in spell):raise ValueError('Configure a magia antes do healing')
        if not self.active(HEAL):return False
        self.bridge.status('Healing: solicitando magia',f'HP atual: {hp:g} | limite absoluto: {s.hpThreshold}')
        result=self.run_native([self.folder/'OTC_Talk_Once.exe','--pid',self.pid,'--thread',self.thread,'--text',spell])
        self.require_native(result);self.last_heal=self.clock()
        self.record('healing_requested',{'hp':hp,'spell':spell,'delivery_confirmed':False})
        return True
    def commands(self,s):
        if s.addSerial!=self.add_serial:
            self.add_serial=s.addSerial;p=self.world.player()
            with self.bridge.lock() as state:
                if state.flags&CAVE:raise ValueError('Desative o cavebot antes de editar a rota')
                if state.waypointCount>=64:raise ValueError('Limite de 64 waypoints')
                state.waypoints[state.waypointCount]=Position(p['x'],p['y'],p['z']);state.waypointCount+=1
        if s.clearSerial!=self.clear_serial:
            self.clear_serial=s.clearSerial
            with self.bridge.lock() as state:
                state.flags&=~CAVE;state.waypointCount=0;state.waypointIndex=0
            self.goal=None
        if s.removeSerial!=self.remove_serial:
            self.remove_serial=s.removeSerial
            with self.bridge.lock() as state:
                if state.flags&CAVE:raise ValueError('Desative o cavebot antes de editar a rota')
                if s.removeIndex<state.waypointCount:
                    for i in range(s.removeIndex,state.waypointCount-1):state.waypoints[i]=state.waypoints[i+1]
                    state.waypointCount-=1;state.waypointIndex=0
            self.goal=None
    def pulse(self):
        self.memory.alive();s=self.state();self.commands(s);self.save_settings(s)
        self.service_light(s)
        if not s.flags&CAVE:self.goal=None
        p=self.world.player();hp=self.health()
        self.bridge.status(self.phase,f"HP: {hp:g} | {p['x']}, {p['y']}, {p['z']}")
        return self.heal(s)
    def waypoint(self,s):
        if not s.waypointCount:self.bridge.disable(CAVE);self.phase='Adicione waypoints antes de ativar o cavebot';return
        index=s.waypointIndex if s.waypointIndex<s.waypointCount else 0
        p=s.waypoints[index];destination={'x':p.x,'y':p.y,'z':p.z};current=self.world.player()
        if current['z']!=p.z:raise ValueError('Waypoint em outro andar; mudancas de andar ainda nao configuradas')
        if current==destination:
            next_index=index+1
            if next_index>=s.waypointCount:
                next_index=0
                if not s.flags&LOOP:self.bridge.disable(CAVE);self.phase='Rota concluida'
            with self.bridge.lock() as state:state.waypointIndex=next_index
            self.goal=None;return
        if self.goal:
            if self.clock()-self.goal['started']>60:raise ValueError('Waypoint nao atingido em 60 segundos; nao repetiu a rota')
            self.phase=f'Cavebot: indo ao waypoint {index+1}/{s.waypointCount}';return
        if abs(current['x']-p.x)+abs(current['y']-p.y)>100:raise ValueError('Waypoint acima de 100 SQMs; grave pontos mais proximos')
        # Nao disputa movimento com ataque/follow que ja esteja ativo no cliente.
        if self.world.u64(self.base+0xE52A50) or self.world.u64(self.base+0xE52A60):
            self.phase='Cavebot aguardando termino do ataque/follow';return
        self.phase=f'Cavebot: waypoint {index+1}/{s.waypointCount}'
        if not self.active(CAVE):return
        result=self.run_native([self.folder/'OTC_AutoWalk_Once.exe','--pid',self.pid,'--thread',self.thread,
                                '--x',p.x,'--y',p.y,'--z',p.z])
        self.require_native(result);self.goal={'started':self.clock(),'position':destination}
    def step(self):
        if self.pulse():return
        s=self.state()
        if s.flags&ATTACK:
            if self.world.u64(self.base+0xE52A50) or self.world.u64(self.base+0xE52A60):
                self.phase='Aguardando ataque/follow ja ativo';return
            if self.world.rats_on_floor():
                report={};self.phase='Ataque: acompanhando o mesmo Rat'
                try:
                    cycle(self.memory,self.world,self.pid,self.thread,s.destination,60,report,
                          lambda:not self.active(ATTACK) or ('loot' in report and not self.active(LOOT)),
                          runner=self.run_native,pulse=self.pulse,collect=lambda:self.active(LOOT))
                except SkipCycle as error:
                    report.update(status='cycle_skipped',confirmed=False,skip_reason=str(error))
                    raise
                finally:
                    self.record('attack_loot_cycle',report)
                    if report.get('new_corpse'):self.handled.add(report['new_corpse']['object'])
                self.goal=None
                self.phase=('Alvo fora do mapa carregado; aguardando novo Rat.' if report.get('status')=='target_lost'
                            else 'Ciclo concluido. Procurando outro Rat.')
                return
            self.phase='Ataque: aguardando Rat vivo no mesmo andar'
        if s.flags&LOOT and not s.flags&ATTACK and self.clock()-self.last_scan>=1:
            self.last_scan=self.clock();bodies=[c for c in self.world.nearby_corpses() if c['object'] not in self.handled]
            if len(bodies)==1:
                body=bodies[0];report={};self.phase='Loot: abrindo corpo adjacente'
                try:
                    open_and_collect(self.memory,self.world,self.pid,self.thread,s.destination,report,
                                     lambda:not self.active(LOOT),body['position'],int(body['object'],16),runner=self.run_native,pulse=self.pulse)
                except SkipCycle as error:
                    report.update(status='cycle_skipped',confirmed=False,skip_reason=str(error))
                    raise
                finally:
                    self.record('standalone_loot',report);self.handled.add(body['object'])
                self.phase='Loot concluido';return
        if s.flags&CAVE:self.waypoint(s)
        elif not s.flags&ACTION_MASK:self.goal=None;self.phase='Automacoes desligadas'
        elif s.flags&LIGHT and not s.flags&(ATTACK|LOOT|CAVE|HEAL):self.phase='Luz externa ativa (experimental)'
    def service_light(self,s):
        try:self.light.tick(bool(s.flags&LIGHT),s.lightLevel)
        except Exception as error:
            self.bridge.disable(LIGHT)
            self.record('light_error',{'error':str(error)})
            restore_failed=None
            try:self.light.disable()
            except Exception as restore_error:
                restore_failed=str(restore_error);self.record('light_restore_error',{'error':restore_failed})
            self.light.status=f'Luz desativada: {error}'+(f'. Falha ao restaurar: {restore_failed}' if restore_failed else '')
        finally:
            with self.bridge.lock() as state:
                state.lightStatus=self.light.status;state.lightObserved=self.light.observed
    def close(self):
        try:return self.light.close()
        finally:
            with self.bridge.lock() as s:s.lightStatus=self.light.status;s.lightObserved=self.light.observed
    def run(self):
        self.record('controller_started',{'pid':self.pid,'thread':self.thread,'base':hex(self.base)})
        while not self.quit:
            try:
                if self.fault:
                    self.bridge.disable();self.service_light(self.bridge.snapshot());self.bridge.status('Automacoes interrompidas',self.fault)
                    if self.bridge.snapshot().uiClosed:break
                else:self.step()
            except StopRequested:break
            except SkipCycle as error:
                self.goal=None;self.phase='Ciclo ignorado. Continuando a busca.'
                self.record('cycle_skipped',{'reason':str(error)})
                self.bridge.status(self.phase,str(error))
                time.sleep(0.85)
            except Exception as error:
                try:s=self.bridge.snapshot()
                except Exception:raise
                if not s.flags&ACTION_MASK or str(error).startswith(('Parado','Parada')):
                    self.phase='Parado; chamadas ja enviadas podem continuar no cliente'
                    self.bridge.status(self.phase,str(error))
                else:
                    self.fault=str(error)[:255];self.bridge.disable();self.record('fault',{'error':str(error)})
            time.sleep(0.15)
