"""Bounded chase recovery for the same, still active attack target."""
import time
from cromin.actions.common import native_completed
from cromin.core.paths import BIN
from cromin.readers.world import RatDataError


class FollowWatch:
    def __init__(self,world,base,pid,thread,runner,enabled,record=lambda *a:None,clock=time.monotonic):
        self.world,self.base,self.pid,self.thread=world,base,pid,thread
        self.runner,self.enabled,self.record,self.clock=runner,enabled,record,clock
        self.selected=None;self.anchor=None;self.since=0;self.last=-1e9
        self.attempts=0;self.disabled=False;self.limit_reported=False

    def clear(self):
        self.selected=None;self.anchor=None;self.attempts=0;self.disabled=False;self.limit_reported=False

    def observe(self,selected,current,player,report=None):
        identity=lambda item:tuple(item.get(k) for k in ('id','object','control','name'))
        if self.selected is None or identity(self.selected)!=identity(selected):
            self.clear();self.selected=dict(selected);self.last=-1e9
        if not self.enabled():
            self.anchor=None;return
        # Never turn a cached/off-map object or a different monster into a follow action.
        if not current or identity(current)!=identity(selected) or current.get('hp',0)<=0:
            self.anchor=None;return
        p=current['position']
        if p['z']!=player['z'] or not (0<p['x']<65535 and 0<p['y']<65535 and 0<=p['z']<=15):
            self.anchor=None;return
        if max(abs(player['x']-p['x']),abs(player['y']-p['y']))<=1:
            self.anchor=None;self.attempts=0;self.limit_reported=False;return
        now=self.clock();anchor=tuple(player[k] for k in ('x','y','z'))
        if anchor!=self.anchor:
            self.anchor=anchor;self.since=now;self.attempts=0;self.limit_reported=False;return
        if self.disabled or now-self.since<2 or now-self.last<3:return
        if self.attempts>=3:
            if not self.limit_reported:
                self.emit({'status':'stationary_retry_limit','target_id':selected['id'],'attempts':3},report)
                self.limit_reported=True
            return
        # A manual switch must never cause us to renew chase for another target.
        if self.world.u64(self.base+0xE52A50)!=int(selected['object'],16):return
        followed=self.world.u64(self.base+0xE52A60)
        if followed not in (0,int(selected['object'],16)):return
        if not self.enabled():return
        self.last=now;self.attempts+=1
        command=[BIN/'OTC_Rat_Attack_Once.exe','--pid',self.pid,'--thread',self.thread,
                 '--id',selected['id'],'--object',selected['object'],
                 '--monster-name-hex',selected.get('name','Rat').encode('utf-8').hex(),'--refresh-chase',1]
        result=self.runner(command,'[JSON] ');data=result.get('report') or {}
        event={'target_id':selected['id'],'attempt':self.attempts,'player':dict(player),'result':result}
        # Explicit preflight skip means the native helper never issued a call.
        if data.get('refresh_preflight_skipped') and data.get('follow_refresh_only') and not data.get('started') and data.get('detached') and not data.get('diagnostics'):
            self.emit(dict(event,status='preflight_skipped'),report);return
        restored=all(data.get(k) for k in ('returned','context_restored','stack_slot_restored','detached'))
        if not restored:
            raise RuntimeError('Renovacao do Follow sem restauracao confirmada; nao repete')
        if data.get('attack_requested') or not data.get('follow_refresh_only'):
            raise RuntimeError('Relatorio inesperado na renovacao do Follow; nao repete')
        if not native_completed(result,('follow_refresh_returned','follow_refresh_reset_verified','chase_verified')):
            self.disabled=True;self.emit(dict(event,status='refresh_refused_no_retry'),report);return
        if data.get('rat_id')!=selected['id'] or data.get('rat_object')!=selected['object']:
            raise RuntimeError('Identidade divergente no retorno do Follow')
        self.emit(dict(event,status='chase_renewed_movement_not_confirmed'),report)

    def emit(self,event,report):
        self.record('follow_recovery',event)
        if report is not None:report.setdefault('follow_recovery',[]).append(event)

    def tick_active(self):
        selected=self.selected
        if not selected:return
        if self.world.u64(self.base+0xE52A50)!=int(selected['object'],16):
            self.clear();return
        try:current=self.world.monster(selected['id'])
        except RatDataError as error:
            # No action can use this read. Discard it; never infer a corpse/death here.
            self.emit({'status':'active_target_read_skipped','details':error.details},None)
            self.clear();return
        self.observe(selected,current,self.world.player())
