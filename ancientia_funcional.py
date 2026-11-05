"""Cromin Ancientia: ataque por hotkey, cavebot e luz experimental."""
import argparse
from datetime import datetime,timezone
import json
from pathlib import Path
import sys
import time
import threading
import queue
import tkinter as tk
from tkinter import ttk,messagebox
sys.path.insert(0,str(Path(__file__).resolve().parent/'src'))
from cromin.readers.memory import Memory
from cromin.readers.ancientia import DX
from cromin.readers.ancientia_pending import connect
from cromin.readers.ancientia_actions import read_state,validate_action_readers,light_candidates,scan_monsters,read_monster
from cromin.core.ancientia_runner import Runner,KEYS
from cromin.core.ancientia_loot import CorpseLoot
from cromin.readers.ancientia_loot import inventory
from cromin.actions.ancientia_light import AncientiaLight
from cromin.actions.ancientia_mouse import Mouse
from cromin.readers.ancientia_profile import LIGHT_WRITE_ALLOWED

ROOT=Path(__file__).resolve().parent


class Panel:
    def __init__(self,window,pid,expected_name=''):
        self.window=window;self.memory=None;self.base=None;self.keyboard=None;self.runner=None;self.sample=None
        self.identity=None;self.light_states=[];self.light=None;self.loot=None;self.manual_epoch=0
        self.calibration=None;self.center_point=None;self.monsters=[]
        self.scan_thread=None;self.scan_cancel=threading.Event();self.scan_queue=queue.Queue()
        self.generation=0;self.next_scan=0
        self.expected_name=expected_name
        self.pid=tk.StringVar(value=str(pid or ''))
        self.summary=tk.StringVar(value='Abra o Ancientia pelo launcher e conecte o PID do DX.')
        self.status=tk.StringVar(value='Desconectado')
        self.repeat=tk.BooleanVar(value=False)
        self.attack_mode=tk.StringVar(value='Hotkey')
        self.attack_key=tk.StringVar(value='SPACE')
        self.hotkey_confirmed=tk.BooleanVar(value=False)
        self.light_enabled=tk.BooleanVar(value=False)
        self.monster_names=tk.StringVar(value='')
        self.calibration_status=tk.StringVar(value='Mapa nao calibrado.')
        self.report=ROOT/'reports'/('Ancientia_acoes_'+datetime.now().strftime('%Y%m%d_%H%M%S_%f')+'.jsonl')
        character_key=''.join(c if c.isalnum() else '_' for c in (expected_name or 'personagem'))
        self.settings=ROOT/'data'/f"Ancientia_{character_key}_{DX['sha256'][:8]}.settings.json"
        window.title('Cromin Â· Ancientia DX');window.geometry('640x570')
        frame=ttk.Frame(window,padding=12);frame.pack(fill='both',expand=True)
        row=ttk.Frame(frame);row.pack(fill='x')
        ttk.Label(row,text='PID DX:').pack(side='left');ttk.Entry(row,textvariable=self.pid,width=10).pack(side='left',padx=6)
        ttk.Button(row,text='Conectar',command=self.connect).pack(side='left')
        ttk.Button(row,text='PARAR Â· F8',command=self.stop).pack(side='right')
        ttk.Label(frame,textvariable=self.summary,wraplength=610).pack(fill='x',pady=10)
        ttk.Label(frame,textvariable=self.status,wraplength=610).pack(fill='x')
        tabs=ttk.Notebook(frame);self.tabs=tabs;tabs.pack(fill='both',expand=True,pady=10)
        cave=ttk.Frame(tabs,padding=10);attack=ttk.Frame(tabs,padding=10);light=ttk.Frame(tabs,padding=10)
        tabs.add(cave,text='Cavebot');tabs.add(attack,text='Ataque');tabs.add(light,text='Luz')
        loot=ttk.Frame(tabs,padding=10);tabs.add(loot,text='Loot')
        ttk.Label(loot,text='Auto loot do Ancientia: bag 0 principal. Ao perder o alvo, tenta abrir a ultima posicao e os oito SQMs ao redor do personagem. Nao arrasta itens nem se aproxima de corpos distantes.',wraplength=570).pack(anchor='w',pady=8)
        ttk.Button(loot,text='1. Calibrar centro do personagem (5s)',command=lambda:self.calibrate('center')).pack(anchor='w',pady=5)
        ttk.Button(loot,text='2. Calibrar SQM a direita (5s)',command=lambda:self.calibrate('tile')).pack(anchor='w',pady=5)
        ttk.Label(loot,textvariable=self.calibration_status,wraplength=570).pack(anchor='w')
        ttk.Button(loot,text='Ativar varredura de corpos',command=self.start_loot).pack(anchor='w',pady=10)
        ttk.Button(loot,text='Varrer os 8 SQMs atuais em 5 segundos',command=self.manual_sweep).pack(anchor='w',pady=5)
        ttk.Label(loot,text='Abra a bag 0 e feche outros corpos antes de iniciar. F8 desliga. Recalibre se mudar zoom ou paineis.',wraplength=570).pack(anchor='w')
        ttk.Label(cave,text='Grave pontos proximos no mesmo andar. Setas do jogo precisam caminhar.').pack(anchor='w')
        self.route=tk.Listbox(cave,height=9);self.route.pack(fill='x',pady=6)
        row=ttk.Frame(cave);row.pack(fill='x')
        ttk.Button(row,text='Gravar posicao atual',command=self.add).pack(side='left')
        ttk.Button(row,text='Remover selecionado',command=self.remove).pack(side='left',padx=5)
        ttk.Checkbutton(cave,text='Repetir rota',variable=self.repeat).pack(anchor='w',pady=6)
        ttk.Button(cave,text='Iniciar cavebot',command=self.start_cave).pack(anchor='w')
        ttk.Label(attack,text='Modo de ataque:').pack(anchor='w')
        mode=ttk.Combobox(attack,textvariable=self.attack_mode,values=['Hotkey','Mouse'],state='readonly',width=15)
        mode.pack(anchor='w',pady=5);mode.bind('<<ComboboxSelected>>',self.change_attack_mode)
        self.hotkey_frame=ttk.Frame(attack)
        ttk.Label(self.hotkey_frame,text='Escolha a tecla que funciona NO JOGO para selecionar e atacar monstros. SPACE e uma opcao inicial, nao uma tecla confirmada.',wraplength=570).pack(anchor='w')
        ttk.Combobox(self.hotkey_frame,textvariable=self.attack_key,values=[k for k in KEYS if k not in ('LEFT','RIGHT','UP','DOWN')],state='readonly',width=10).pack(anchor='w',pady=10)
        ttk.Checkbutton(self.hotkey_frame,text='Conferi que esta tecla seleciona e ataca monstros; chat fechado',variable=self.hotkey_confirmed).pack(anchor='w')
        ttk.Label(self.hotkey_frame,text='Sem calibracao do mouse. A selecao segue o cliente; filtro por nome nao se aplica neste modo.',wraplength=570).pack(anchor='w',pady=8)
        ttk.Button(self.hotkey_frame,text='Iniciar ataque por hotkey',command=self.start_attack).pack(anchor='w')
        self.mouse_frame=ttk.Frame(attack)
        ttk.Label(self.mouse_frame,text='Alternativa: botao direito. Calibre o mapa usando os dois botoes.',wraplength=570).pack(anchor='w')
        ttk.Button(self.mouse_frame,text='1. Centro do SQM do personagem (5s)',command=lambda:self.calibrate('center')).pack(anchor='w',pady=5)
        ttk.Button(self.mouse_frame,text='2. Centro do SQM a direita (5s)',command=lambda:self.calibrate('tile')).pack(anchor='w',pady=5)
        ttk.Label(self.mouse_frame,textvariable=self.calibration_status,wraplength=570).pack(anchor='w')
        ttk.Label(self.mouse_frame,text='Nomes separados por virgula (vazio = qualquer Monster):').pack(anchor='w',pady=5)
        ttk.Entry(self.mouse_frame,textvariable=self.monster_names,width=45).pack(anchor='w')
        ttk.Button(self.mouse_frame,text='Localizar monstros',command=self.request_scan).pack(anchor='w',pady=5)
        ttk.Button(self.mouse_frame,text='Iniciar ataque por mouse',command=self.start_attack).pack(anchor='w')
        self.hotkey_frame.pack(fill='x',pady=5)
        self.attack_key.trace_add('write',self.change_attack_key)
        ttk.Checkbutton(light,text='Testar luz: intensidade 31 (F8 restaura)',variable=self.light_enabled,command=self.toggle_light,state='normal' if LIGHT_WRITE_ALLOWED else 'disabled').pack(anchor='w',pady=8)
        ttk.Label(light,text='A coleta da build atual confirmou 0/7/0/7 em 0xC9. Marque para testar o efeito visual; desmarque ou use F8 para restaurar.',wraplength=570).pack(anchor='w')
        self.light_label=tk.StringVar(value='1/4: desligue/remova a fonte de luz e capture.')
        ttk.Label(light,textvariable=self.light_label,wraplength=570).pack(anchor='w',pady=15)
        ttk.Button(light,text='Capturar estado da luz',command=self.capture_light).pack(anchor='w')
        ttk.Button(light,text='Reiniciar coleta',command=self.reset_light).pack(anchor='w',pady=10)
        ttk.Label(light,text='O teste altera somente a intensidade em 0xC9 e preserva a cor em 0xC8. Os botoes de coleta continuam somente leitura.',wraplength=570).pack(anchor='w')
        ttk.Label(frame,text='Teclas enviadas apenas com o PID do Ancientia em primeiro plano.').pack(anchor='w')
        self.waypoints=[]
        if self.settings.exists():
            try:
                saved=json.loads(self.settings.read_text(encoding='utf-8'))
                for p in saved.get('route',[]):
                    if (not isinstance(p,list) or len(p)!=3 or any(type(v)!=int for v in p)
                        or not 0<p[0]<65535 or not 0<p[1]<65535 or not 0<=p[2]<=15):
                        raise ValueError('Waypoint salvo invalido')
                if len(saved.get('route',[]))>128:raise ValueError('Rota acima do limite')
                self.waypoints=saved.get('route',[]);self.refresh_route()
            except (ValueError,OSError,KeyError) as e:self.status.set(f'Rota nao carregada: {e}')
        window.protocol('WM_DELETE_WINDOW',self.close)
        window.after(250,self.tick)

    def record(self,event,data):
        self.report.parent.mkdir(exist_ok=True)
        with self.report.open('a',encoding='utf-8') as stream:
            stream.write(json.dumps(dict(utc=datetime.now(timezone.utc).isoformat(),event=event,data=data),ensure_ascii=False)+'\n')

    def guarded(self,fn):
        try:return fn()
        except (RuntimeError,ValueError,OSError,KeyError) as error:
            self.stop(str(error));messagebox.showerror('Cromin',str(error));return None

    def connect(self):
        def run():
            self.disconnect()
            pid=int(self.pid.get())
            if pid<=0:raise ValueError('PID precisa ser positivo')
            self.memory=Memory(pid);self.base=connect(self.memory)
            validate_action_readers(self.memory,self.base)
            self.sample=read_state(self.memory,self.base)
            if self.expected_name and (self.sample['name'] or '').casefold()!=self.expected_name.casefold():
                raise ValueError(f"Este PID esta com {self.sample['name']}, mas o personagem escolhido e {self.expected_name}.")
            self.identity=(self.sample['player'],self.sample['name'])
            self.keyboard=Mouse(pid);self.runner=Runner(self.keyboard.send,self.record)
            self.loot=CorpseLoot(self.inspect_dead,lambda:inventory(self.memory,self.base),lambda player,pos:self.keyboard.right_click(self.calibration,player,pos),self.record)
            self.light=AncientiaLight(self.memory,self.base,pid,self.record)
            self.status.set('Conectado. Automacoes desligadas.')
            self.record('connected',dict(pid=pid,base=hex(self.base),sample=self.sample))
        def safe_connect():
            try:run()
            except (RuntimeError,ValueError,OSError,KeyError):
                if self.memory:self.memory.close()
                self.memory=None;self.runner=None;self.keyboard=None;self.sample=None
                raise
        self.guarded(safe_connect)

    def manual_sweep(self):
        def begin():
            self.need_connection()
            if not self.calibration:raise ValueError('Calibre os dois pontos do mapa primeiro.')
            self.stop('Volte ao jogo: varredura em 5 segundos')
            generation=self.generation;epoch=self.manual_epoch
            def finish():
                if generation!=self.generation or epoch!=self.manual_epoch:return
                def run():
                    self.need_connection()
                    if not self.keyboard.focused() or self.keyboard.stop_pressed():
                        raise ValueError('Varredura cancelada: jogo fora de foco ou F8 pressionado.')
                    state=read_state(self.memory,self.base)
                    if (state['player'],state['name'])!=self.identity:raise ValueError('Personagem mudou.')
                    self.loot.sweep_here(state,time.monotonic())
                self.guarded(run)
            self.window.after(5000,finish)
        self.guarded(begin)

    def inspect_dead(self,address):
        try:return read_monster(self.memory,self.base,int(address,0),allow_dead=True)
        except (OSError,RuntimeError,ValueError):return None

    def start_loot(self):
        def run():
            self.need_connection()
            if not self.calibration:raise ValueError('Calibre os dois pontos do mapa na aba Loot.')
            if not any(b['containerId']==0 for b in inventory(self.memory,self.base)):
                raise ValueError('Abra a bag 0 primeiro.')
            self.loot.reset();self.loot.enabled=True
            self.status.set('Loot armado para corpos adjacentes. Volte ao jogo; F8 para parar.')
            self.record('loot_armed',{'mainBag':0})
        self.guarded(run)

    def need_connection(self):
        if self.runner is None or self.memory is None:raise ValueError('Conecte primeiro.')
        if self.expected_name and (self.sample['name'] or '').casefold()!=self.expected_name.casefold():
            raise ValueError('O personagem mudou; conecte o PID do personagem escolhido.')

    def toggle_light(self):
        def run():
            self.need_connection()
            if self.light_enabled.get() and not LIGHT_WRITE_ALLOWED:
                raise ValueError('A escrita de luz precisa de validacao para esta build.')
            if self.light:self.light.tick(self.light_enabled.get(),31)
            self.status.set('Luz aplicada; confira o efeito visual.' if self.light_enabled.get() else 'Luz desligada/restaurada.')
        self.guarded(run)

    def stop(self,reason='Parado por F8 ou botao'):
        self.manual_epoch=getattr(self,'manual_epoch',0)+1
        if self.runner:self.runner.stop(reason)
        if self.loot:self.loot.reset()
        self.scan_cancel.set()
        self.light_enabled.set(False)
        if self.light:
            try:self.light.disable()
            except (OSError,RuntimeError,ValueError) as error:
                reason=f'{reason}. Falha ao restaurar luz: {error}'
        self.status.set(reason)

    def disconnect(self):
        self.stop('Desconectado')
        self.generation+=1;self.monsters=[];self.calibration=None;self.center_point=None
        self.scan_thread=None
        if self.light:self.light.close()
        self.light=None
        if self.memory:self.memory.close()
        self.memory=None;self.runner=None;self.keyboard=None;self.sample=None;self.identity=None
        self.reset_light()

    def refresh_route(self):
        self.route.delete(0,'end')
        for i,p in enumerate(self.waypoints):self.route.insert('end',f'{i+1}: {p[0]}, {p[1]}, {p[2]}')

    def save_route(self):
        self.settings.parent.mkdir(exist_ok=True)
        self.settings.write_text(json.dumps(dict(route=self.waypoints),indent=2),encoding='utf-8')
        self.refresh_route()

    def add(self):
        def run():
            self.need_connection();self.stop('Gravando rota')
            sample=read_state(self.memory,self.base)
            if len(self.waypoints)>=128:raise ValueError('Limite de 128 waypoints')
            self.waypoints.append(list(sample['position']));self.save_route()
        self.guarded(run)

    def remove(self):
        def run():
            self.stop('Editando rota')
            selection=self.route.curselection()
            if selection:self.waypoints.pop(selection[0]);self.save_route()
        self.guarded(run)

    def start_cave(self):
        def run():
            self.need_connection()
            if not self.waypoints:raise ValueError('Grave pelo menos um waypoint.')
            self.runner.route=[tuple(p) for p in self.waypoints];self.runner.index=0
            self.runner.loop=self.repeat.get();self.runner.cave=True
            self.runner.pending_step=None;self.runner.failures=0
            self.status.set('Cavebot armado. Volte a janela do jogo; F8 para parar.')
        self.guarded(run)

    def start_attack(self):
        def run():
            self.need_connection()
            mode=self.attack_mode.get()
            if mode=='Hotkey':
                key=self.attack_key.get()
                if key not in KEYS or key in ('LEFT','RIGHT','UP','DOWN'):
                    raise ValueError('Selecione uma hotkey de ataque valida. F8 fica reservado para parar.')
                if not self.hotkey_confirmed.get():
                    raise ValueError('Confira a tecla de selecionar/atacar monstros no jogo e marque a opcao.')
                self.runner.attack_action=None
                self.runner.attack_key=key
                self.scan_cancel.set()
            elif mode=='Mouse':
                if not self.calibration:raise ValueError('Calibre os dois pontos do mapa primeiro.')
                self.runner.attack_action=self.click_monster
            else:raise ValueError('Modo de ataque invalido.')
            self.runner.attack=True
            self.runner.attack_failures=0;self.runner.pending_attack=None;self.runner.next_attack=0;self.runner.expected_target=None
            if mode=='Mouse':self.request_scan()
            self.record('attack_armed',dict(mode=mode,key=self.runner.attack_key if mode=='Hotkey' else None))
            self.status.set(f'Ataque por {mode.lower()} armado. Volte ao jogo; F8 para parar.')
        self.guarded(run)

    def change_attack_mode(self,event=None):
        if self.runner:
            self.runner.attack=False;self.runner.pending_attack=None;self.runner.expected_target=None
        self.scan_cancel.set()
        self.hotkey_frame.pack_forget();self.mouse_frame.pack_forget()
        (self.hotkey_frame if self.attack_mode.get()=='Hotkey' else self.mouse_frame).pack(fill='x',pady=5)
        self.status.set('Modo alterado. Clique Iniciar ataque quando estiver pronto.')

    def change_attack_key(self,*args):
        self.hotkey_confirmed.set(False)
        if self.runner:
            self.runner.attack=False;self.runner.pending_attack=None;self.runner.expected_target=None
        self.status.set('Tecla alterada. Confira-a no jogo e inicie o ataque novamente.')

    def calibrate(self,phase):
        def begin():
            self.need_connection();self.stop('Calibrando mapa')
            generation=self.generation
            self.calibration_status.set('Volte ao jogo e deixe o mouse no centro do SQM '+('do personagem.' if phase=='center' else 'imediatamente a DIREITA do personagem.'))
            def capture():
                if generation!=self.generation:return
                def finish():
                    point=self.keyboard.capture_point()
                    if phase=='center':
                        self.center_point=point;self.calibration=None
                        self.calibration_status.set('Centro gravado. Agora calibre o SQM a direita.')
                    else:
                        center=self.center_point
                        if not center or center['hwnd']!=point['hwnd'] or center['rect']!=point['rect']:
                            raise ValueError('Calibre o centro primeiro, sem redimensionar o jogo.')
                        dx=point['point'][0]-center['point'][0];dy=point['point'][1]-center['point'][1]
                        if not 8<=dx<=128 or abs(dy)>max(3,dx/4):
                            raise ValueError('Ponto precisa ser o centro do SQM imediatamente a direita.')
                        self.calibration=dict(hwnd=point['hwnd'],rect=point['rect'],center=center['point'],tile=dx)
                        self.calibration_status.set(f'Mapa calibrado: SQM de {dx}px. Recalibre se mover paineis ou zoom.')
                        self.record('map_calibrated',self.calibration)
                self.guarded(finish)
            self.window.after(5000,capture)
        self.guarded(begin)

    def request_scan(self):
        def run():
            self.need_connection()
            if self.scan_thread and self.scan_thread.is_alive():return
            self.scan_cancel=threading.Event();cancel=self.scan_cancel
            generation=self.generation;pid=self.memory.pid
            self.next_scan=time.monotonic()+30
            def work():
                memory=None
                try:
                    memory=Memory(pid);base=connect(memory);validate_action_readers(memory,base)
                    last=[0.0]
                    def progress(count,stats):
                        if time.monotonic()-last[0]>3:
                            self.scan_queue.put((generation,'progress',dict(count=count,readBytes=stats['read_bytes'])))
                            last[0]=time.monotonic()
                    found,stats=scan_monsters(memory,base,cancel.is_set,progress)
                    self.scan_queue.put((generation,'result',dict(monsters=found,stats=stats)))
                except (OSError,RuntimeError,ValueError) as error:
                    self.scan_queue.put((generation,'error',str(error)))
                finally:
                    if memory:memory.close()
            self.scan_thread=threading.Thread(target=work,daemon=True);self.scan_thread.start()
            self.status.set('Localizando instancias Monster em segundo plano...')
        self.guarded(run)

    def click_monster(self):
        if not self.calibration or not self.keyboard.focused():return False
        player=read_state(self.memory,self.base)
        if player['target'] and player['target']['hpPercent']>0:return False
        names={s.strip().casefold() for s in self.monster_names.get().split(',') if s.strip()}
        choices=[]
        for cached in self.monsters:
            try:monster=read_monster(self.memory,self.base,int(cached['address'],0))
            except (OSError,RuntimeError):continue
            if not monster or monster['name']!=cached['name']:continue
            if names and monster['name'].casefold() not in names:continue
            x,y,z=monster['position'];px,py,pz=player['position']
            if z!=pz or abs(x-px)>4 or abs(y-py)>3 or (x,y)==(px,py):continue
            choices.append((abs(x-px)+abs(y-py),monster))
        if not choices:return False
        monster=min(choices,key=lambda item:item[0])[1]
        if self.keyboard.right_click(self.calibration,player['position'],monster['position']):return monster
        return False

    def reset_light(self):
        self.light_states=[]
        if hasattr(self,'light_label'):self.light_label.set('1/4: desligue/remova a fonte de luz e capture.')

    def capture_light(self):
        def run():
            self.need_connection();self.stop('Capturando luz')
            if len(self.light_states)>=4:raise ValueError('Coleta concluida; reinicie para outra sequencia.')
            state=read_state(self.memory,self.base)
            obj=int(state['player'],0)
            first=self.memory.exact(obj,0x500);second=self.memory.exact(obj,0x500)
            verify=read_state(self.memory,self.base)
            if verify['player']!=state['player'] or verify['position']!=state['position']:
                raise ValueError('Jogador/posicao mudou durante captura.')
            self.light_states.append(dict(player=state['player'],position=state['position'],first=first.hex(),second=second.hex()))
            report=dict(schema='ancientia-light-transition-v1',pid=self.memory.pid,sha256=DX['sha256'],
                        moduleBase=hex(self.base),states=self.light_states,candidates=[],writesPerformed=False)
            if len(self.light_states)==4:report['candidates']=light_candidates(self.light_states)
            output=ROOT/'reports'/f'Ancientia_luz_{datetime.now().strftime("%Y%m%d_%H%M%S_%f")}.json'
            output.parent.mkdir(exist_ok=True);output.write_text(json.dumps(report,indent=2),encoding='utf-8')
            labels=['2/4: ligue/equipe a fonte de luz e capture.','3/4: desligue/remova novamente e capture.',
                    '4/4: ligue/equipe novamente e capture.',f'Concluido. Envie {output.name} de reports.']
            self.light_label.set(labels[len(self.light_states)-1]);self.status.set(str(output))
        self.guarded(run)

    def tick(self):
        try:
            while True:
                try:generation,event,data=self.scan_queue.get_nowait()
                except queue.Empty:break
                if generation!=self.generation:continue
                if event=='result' and not data['stats'].get('cancelled'):
                    self.monsters=data['monsters'];self.record('monster_scan',data)
                    self.next_scan=time.monotonic()+30
                    self.status.set(f'{len(self.monsters)} instancias Monster localizadas; alvo e revalidado antes do clique.')
                elif event=='error':self.stop('Busca de monstros falhou: '+data)
                elif event=='progress':self.calibration_status.set(f"Busca: {data['readBytes']/1048576:.0f} MiB lidos; {data['count']} monstros")
            if self.memory and self.runner and self.keyboard:
                if self.keyboard.stop_pressed():self.stop()
                state=read_state(self.memory,self.base)
                identity=(state['player'],state['name'])
                if identity!=self.identity:
                    self.stop('Jogador mudou. Rearme as automacoes quando estiver pronto.');self.identity=identity
                if self.light:self.light.tick(self.light_enabled.get(),31)
                if self.runner.attack and self.runner.attack_action is not None and time.monotonic()>=self.next_scan:self.request_scan()
                self.sample=state
                target=state['target']
                description=(target.get('name') or target['className']) if target else 'nenhum'
                self.summary.set(f"{state['name']} | HP {state['health']:g}/{state['maxHealth']:g} | Mana {state['mana']:g}\nPosicao {state['position']} | Alvo: {description}")
                loot_busy=self.loot.tick(state,self.keyboard.focused(),time.monotonic()) if self.loot else False
                if loot_busy:
                    self.status.set(self.loot.status)
                if not loot_busy and (self.runner.cave or self.runner.attack or self.runner.pending_step or self.runner.pending_attack is not None):
                    self.runner.tick(state,self.keyboard.focused(),time.monotonic())
                    self.status.set(self.runner.status)
        except (RuntimeError,ValueError,OSError,KeyError) as error:
            self.stop(str(error))
        self.window.after(250,self.tick)

    def close(self):
        self.disconnect();self.window.destroy()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid',type=int)
    parser.add_argument('--personagem',default='')
    args=parser.parse_args()
    window=tk.Tk();Panel(window,args.pid,args.personagem);window.mainloop()


if __name__=='__main__':main()
