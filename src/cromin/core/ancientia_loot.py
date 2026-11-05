"""Tenta abrir a ultima posicao adjacente do alvo; cliente faz auto loot."""
class CorpseLoot:
    def __init__(self, inspect, bags, click, record=lambda *args:None):
        self.inspect,self.bags,self.click,self.record=inspect,bags,click,record
        self.reset()

    def reset(self):
        self.enabled=False;self.last=None;self.pending=None;self.seen=set();self.lost_at=None
        self.remaining=[];self.origin=None;self.deadline=0
        self.status='Loot desligado'

    def tick(self,sample,focused,now):
        if not self.enabled:return False
        if sample['health']<=0:
            self.reset();return False
        target=sample['target']
        if target and target['className']=='Monster' and target['hpPercent']>0:
            self.pending=None;self.remaining=[]
            self.last=dict(target);self.lost_at=None;return False
        if target and target['className']!='Monster':
            self.last=None;self.lost_at=None;self.pending=None;self.remaining=[];return False
        if self.pending:
            started,before=self.pending
            current=self.bags()
            changed=[b for b in current if b['containerId']!=0 and
                     (b['containerId'],b['address']) not in before]
            if changed:
                self.pending=None;self.remaining=[];self.status='Recipiente aberto; auto loot do cliente assume'
                self.record('corpse_open_observed',changed);return True
            if tuple(sample['position'])!=self.origin:
                self.pending=None;self.remaining=[];self.status='Personagem moveu; varredura cancelada';return False
            if now>=self.deadline:
                self.pending=None;self.remaining=[]
                self.status='Varredura encerrada sem abertura confirmada'
                self.record('corpse_open_unconfirmed',{});return False
            if self.remaining and now-started>=0.4:
                if not focused:return True
                pos=self.remaining[0]
                if self.click(self.origin,pos):
                    self.remaining.pop(0);self.pending=(now,before)
                    self.record('corpse_neighbour_clicked',{'position':pos})
                return True
            if now-started>=2:
                self.pending=None;self.status='Abertura nao confirmada; seguindo sem repetir clique'
                self.record('corpse_open_unconfirmed',{});return False
            self.status='Conferindo abertura do corpo';return True
        if not self.last:return False
        last=self.last
        # O cliente pode liberar o monstro antes da proxima amostra de HP zero.
        dead=target if target and target.get('address')==last['address'] else self.inspect(last['address'])
        confirmed=bool(dead and dead.get('hpPercent')==0 and dead.get('name')==last.get('name'))
        if not confirmed:
            if target:
                self.last=None;return False
            if self.lost_at is None:self.lost_at=now
            if now-self.lost_at<0.6:
                self.status='Alvo desapareceu; aguardando antes de tentar abrir';return True
            # Inferencia registrada explicitamente; cancelar ataque tambem pode chegar aqui.
        pos=tuple(dead['position'] if confirmed else last['position']);player=tuple(sample['position'])
        token=(last['address'],pos)
        if token in self.seen:self.last=None;return False
        if pos[2]!=player[2] or max(abs(pos[0]-player[0]),abs(pos[1]-player[1]))>1 or pos==player:
            self.last=None;self.status='Corpo fora do alcance adjacente; ignorado'
            self.record('corpse_out_of_reach',{'position':pos,'playerPosition':player});return False
        if not focused:return True
        current=self.bags()
        if not any(b['containerId']==0 for b in current):
            self.reset();self.status='Loot pausado: abra a bag 0';return False
        before={(b['containerId'],b['address']) for b in current if b['containerId']!=0}
        if self.click(player,pos):
            self.seen.add(token)
            if len(self.seen)>256:self.seen={token}
            self.last=None;self.pending=(now,before)
            self.origin=player;self.deadline=now+8
            self.remaining=[(player[0]+dx,player[1]+dy,player[2])
                            for dy in (-1,0,1) for dx in (-1,0,1)
                            if (dx or dy) and (player[0]+dx,player[1]+dy,player[2])!=pos]
            self.status='Abrindo corpo; conferindo recipiente'
            self.record('corpse_open_requested',{'position':pos,'bag':0,
                        'deathConfirmed':confirmed,'reason':'hp_zero' if confirmed else 'target_lost_0.6s'});return True
        return True

    def sweep_here(self,sample,now):
        if sample['health']<=0 or sample['target']:
            raise ValueError('Pare o ataque antes da varredura manual.')
        current=self.bags()
        if not any(b['containerId']==0 for b in current):raise ValueError('Abra a bag 0.')
        self.reset();self.enabled=True;self.origin=tuple(sample['position'])
        x,y,z=self.origin
        self.remaining=[(x+dx,y+dy,z) for dy in (-1,0,1) for dx in (-1,0,1) if dx or dy]
        self.pending=(now-0.4,{(b['containerId'],b['address']) for b in current if b['containerId']!=0})
        self.deadline=now+8
        self.record('corpse_manual_sweep_armed',{'playerPosition':self.origin})
