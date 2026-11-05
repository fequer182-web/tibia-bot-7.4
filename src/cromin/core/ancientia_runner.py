"""Cavebot por teclas e ataque por hotkey do cliente, com leitura de retorno."""
import math

KEYS = {'LEFT':0x25,'UP':0x26,'RIGHT':0x27,'DOWN':0x28,'SPACE':0x20,'TAB':0x09,
        **{f'F{i}':0x6F+i for i in range(1,13) if i != 8}}


class Runner:
    def __init__(self,send,record=lambda *args:None,attack_action=None):
        self.send,self.record=send,record
        self.attack_action=attack_action;self.expected_target=None
        self.route=[];self.index=0;self.loop=False
        self.cave=False;self.attack=False;self.attack_key='SPACE'
        self.pending_step=None;self.pending_attack=None
        self.failures=0;self.attack_failures=0;self.next_action=0;self.next_attack=0
        self.status='Parado'

    def stop(self,reason='Parado'):
        self.cave=self.attack=False
        self.pending_step=self.pending_attack=None
        self.expected_target=None
        self.status=reason
        self.record('stop',{'reason':reason})

    def tick(self,sample,focused,now):
        if sample['health']<=0:
            self.stop('Personagem sem vida');return
        target=sample['target']
        if target and target['className']!='Monster':
            self.stop('Alvo nao e Monster; nenhuma nova tecla enviada');return
        if self.pending_attack is not None:
            if target and target['hpPercent']>0:
                if self.expected_target is not None and target.get('address')!=self.expected_target:
                    self.stop('Alvo retornado difere do monstro escolhido; ataque interrompido');return
                self.pending_attack=None;self.attack_failures=0
                self.record('attack_target_confirmed',target)
            elif now-self.pending_attack>=1.5:
                self.pending_attack=None;self.attack_failures+=1
                if self.attack_failures>=3:
                    self.attack=False
                    self.status='Ataque pausado: tres solicitacoes sem alvo'
                    self.record('attack_paused',{'reason':'tres solicitacoes sem alvo'})
                self.next_attack=now+3
            else:
                self.status='Conferindo alvo do atalho';return
        position=tuple(sample['position'])
        if self.pending_step:
            previous,started=self.pending_step
            if position[2]!=previous[2]:
                self.stop('Mudanca de andar; rota interrompida');return
            if position!=previous:
                self.pending_step=None;self.failures=0
                self.record('step_observed',{'before':previous,'after':position})
            elif now-started>=1.5:
                self.pending_step=None;self.failures+=1
                if self.failures>=3:
                    self.stop('Sem progresso: parede, chat ou tecla nao aceita');return
                self.next_action=now+0.3
            else:
                self.status='Conferindo passo';return
        if not focused:
            self.status='Aguardando Ancientia em primeiro plano';return
        if now<self.next_action:return
        if target and target['hpPercent']>0:
            self.status='Em combate; caminhada pausada';return
        if self.attack and now>=self.next_attack:
            if self.attack_action is None and self.attack_key not in KEYS:
                self.stop('Atalho de ataque invalido');return
            sent=self.attack_action() if self.attack_action else self.send(self.attack_key)
            if sent:
                self.expected_target=sent.get('address') if isinstance(sent,dict) else None
                self.pending_attack=now
                self.next_action=now+0.6
                self.record('attack_mouse_requested' if self.attack_action else 'attack_hotkey_requested',
                            sent if isinstance(sent,dict) else {'key':self.attack_key})
                self.status='Ataque solicitado; conferindo alvo'
                # Nao caminhar antes de validar o alvo solicitado.
                return
        if not self.cave:return
        if not self.route:
            self.stop('Rota vazia');return
        dest=tuple(self.route[self.index])
        if len(dest)!=3 or dest[2]!=position[2]:
            self.stop('Waypoint em outro andar');return
        distance=abs(dest[0]-position[0])+abs(dest[1]-position[1])
        if distance>30:
            self.stop('Waypoint a mais de 30 SQMs; grave pontos proximos');return
        if dest==position:
            self.index+=1
            if self.index>=len(self.route):
                if self.loop:self.index=0
                else:self.cave=False;self.status='Rota concluida'
            return
        dx,dy=dest[0]-position[0],dest[1]-position[1]
        key=('RIGHT' if dx>0 else 'LEFT') if dx else ('DOWN' if dy>0 else 'UP')
        if self.send(key):
            self.pending_step=(position,now);self.next_action=now+0.6
            self.status=f'Indo ao ponto {self.index+1}: {dest}'
            self.record('step_requested',{'key':key,'position':position,'destination':dest})
