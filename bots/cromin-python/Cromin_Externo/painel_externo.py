"""Painel externo do Cromin_py. Nao carrega DLL no Antica."""
import argparse
import tkinter as tk
from tkinter import ttk,messagebox
from session import Session
from ui_ipc import ATTACK,LOOT,CAVE,HEAL,LOOP,LIGHT


class App:
    def __init__(self,root,pid=None,thread=None,preview=False):
        self.root=root;self.pid=pid;self.thread=thread;self.preview=preview
        self.session=Session(pid,thread);self.closing=False;self.destroyed=False
        self.synced=False;self.wp_signature=None;self.action_widgets=[]
        root.title('Cromin | Painel externo'+(' | Previa' if preview else ''))
        root.geometry('550x690');root.minsize(510,660);root.configure(bg='#111821')
        style=ttk.Style(root);style.theme_use('clam')
        style.configure('.',font=('Segoe UI',10),background='#17212d',foreground='#edf2f7')
        style.configure('TFrame',background='#17212d');style.configure('TLabel',background='#17212d')
        style.configure('TNotebook',background='#111821',borderwidth=0)
        style.configure('TNotebook.Tab',padding=(18,10),background='#233142',foreground='#c4d0df')
        style.map('TNotebook.Tab',background=[('selected','#344d68')],foreground=[('selected','#ffffff')])
        style.configure('TButton',padding=8,background='#2c4158',foreground='#ffffff')
        style.map('TButton',background=[('active','#3c5877'),('disabled','#233142')],foreground=[('disabled','#8190a1')])
        style.configure('TCheckbutton',background='#17212d',foreground='#edf2f7',padding=5)
        style.map('TCheckbutton',background=[('active','#223247')])
        style.configure('TEntry',fieldbackground='#0f1823',foreground='#ffffff',insertcolor='#ffffff',padding=5)
        style.configure('Stop.TButton',background='#913d48',foreground='white',font=('Segoe UI',11,'bold'))
        style.map('Stop.TButton',background=[('active','#b34c59')])
        top=ttk.Frame(root,padding=18);top.pack(fill='x',padx=12,pady=(12,8))
        ttk.Label(top,text='CROMIN',font=('Segoe UI',22,'bold')).pack(anchor='w')
        ttk.Label(top,text='Painel externo · Antica Retro',foreground='#a8bdce').pack(anchor='w',pady=(2,14))
        self.status=tk.StringVar(value='Previa: sem conexao com o jogo' if preview else 'Desconectado')
        self.detail=tk.StringVar(value='Confira o painel. Nenhuma acao sera executada.' if preview else 'Conecte para conferir a build e o personagem.')
        ttk.Label(top,textvariable=self.status,font=('Segoe UI',11,'bold'),wraplength=470).pack(anchor='w')
        ttk.Label(top,textvariable=self.detail,foreground='#a8bdce',wraplength=470).pack(anchor='w',pady=(6,12))
        self.connect_button=ttk.Button(top,text='Conectar ao Antica',command=self.connect);self.connect_button.pack(fill='x')
        self.tabs=ttk.Notebook(root);self.tabs.pack(fill='both',expand=True,padx=12,pady=4)
        pages=[]
        for name in ('Ataque','Loot','Cavebot','Healing','Luz'):
            page=ttk.Frame(self.tabs,padding=18);self.tabs.add(page,text=name);pages.append(page)
        self.variables={bit:tk.BooleanVar(value=False) for bit in (ATTACK,LOOT,CAVE,HEAL,LIGHT)}
        self.hp=tk.StringVar(value='80');self.cooldown=tk.StringVar(value='2000');self.spell=tk.StringVar(value='')
        self.destination=tk.StringVar(value='0');self.loop=tk.BooleanVar(value=False)
        self.light_level=tk.StringVar(value='7');self.light_info=tk.StringVar(value='Desligada')
        self.checkbox(pages[0],'Ativar auto ataque',ATTACK)
        self.note(pages[0],'Alvo desta versao: Rat',bold=True)
        self.note(pages[0],'Escolhe o Rat vivo mais proximo no mesmo andar e acompanha o mesmo alvo durante a perseguicao.')
        self.note(pages[0],'Para o teste completo, abra a bag e ative tambem o Loot.')
        self.checkbox(pages[1],'Ativar auto loot',LOOT)
        self.note(pages[1],'Recolher moedas · ID 3031',bold=True)
        self.note(pages[1],'Bag de destino: numero da janela aberta (0 a 63)')
        ttk.Entry(pages[1],textvariable=self.destination,width=9).pack(anchor='w',pady=(0,12))
        self.note(pages[1],'Destino validado: bag 2853. O programa confere a quantidade removida do corpo e recebida na bag.')
        self.checkbox(pages[2],'Ativar cavebot',CAVE)
        self.loop_control=ttk.Checkbutton(pages[2],text='Repetir rota',variable=self.loop,command=self.set_loop)
        self.loop_control.pack(anchor='w');self.action_widgets.append(self.loop_control)
        self.waypoints=tk.Listbox(pages[2],height=6,bg='#0f1823',fg='#edf2f7',selectbackground='#375674',
                                 highlightthickness=0,relief='flat',font=('Consolas',10),exportselection=False)
        self.waypoints.pack(fill='both',expand=True,pady=10)
        row=ttk.Frame(pages[2]);row.pack(fill='x')
        for text,command in [('Adicionar posicao',self.add_waypoint),('Remover',self.remove_waypoint),('Limpar',self.clear_waypoints)]:
            button=ttk.Button(row,text=text,command=command);button.pack(side='left',padx=(0,5));self.action_widgets.append(button)
        self.note(pages[2],'Grave pontos com o cavebot desligado. Caminhada no mesmo andar; ate 100 SQMs por trecho.')
        self.checkbox(pages[3],'Ativar healing por magia',HEAL)
        self.note(pages[3],'Usar quando HP atual for menor ou igual a:')
        ttk.Entry(pages[3],textvariable=self.hp,width=12).pack(anchor='w')
        self.note(pages[3],'Valor absoluto de HP, nao porcentagem.')
        self.note(pages[3],'Texto da magia (ate 15 caracteres ASCII)')
        ttk.Entry(pages[3],textvariable=self.spell,width=30).pack(anchor='w')
        self.note(pages[3],'Intervalo minimo entre tentativas (ms)')
        ttk.Entry(pages[3],textvariable=self.cooldown,width=12).pack(anchor='w')
        self.note(pages[3],'Healing e conferido entre chamadas. O retorno da fala nao confirma que o servidor aceitou a magia.')
        self.checkbox(pages[4],'Ativar luz do personagem',LIGHT)
        self.note(pages[4],'Iluminacao experimental',bold=True)
        self.note(pages[4],'Intensidade selecionada (0 a 31)')
        ttk.Spinbox(pages[4],from_=0,to=31,textvariable=self.light_level,width=10).pack(anchor='w')
        ttk.Label(pages[4],textvariable=self.light_info,wraplength=460,foreground='#a8bdce').pack(anchor='w',pady=14)
        self.note(pages[4],'Clique Aplicar configuracao apos mudar a intensidade. Inicia em 7 e desligada.')
        self.note(pages[4],'Ao desligar ou desconectar, tenta restaurar o valor anterior. Confira o efeito visual no jogo.')
        bottom=ttk.Frame(root,padding=12);bottom.pack(fill='x')
        self.apply_button=ttk.Button(bottom,text='Aplicar configuracao',command=self.apply)
        self.apply_button.pack(fill='x',pady=(0,7));self.action_widgets.append(self.apply_button)
        self.stop_button=ttk.Button(bottom,text='PARAR AUTOMACOES',style='Stop.TButton',command=self.stop_all)
        self.stop_button.pack(fill='x')
        self.topmost=tk.BooleanVar(value=True)
        ttk.Checkbutton(bottom,text='Manter painel visivel',variable=self.topmost,
                        command=lambda:root.attributes('-topmost',self.topmost.get())).pack(anchor='w',pady=(6,0))
        root.attributes('-topmost',True);root.protocol('WM_DELETE_WINDOW',self.close)
        self.refresh()
    def note(self,parent,text,bold=False):
        ttk.Label(parent,text=text,wraplength=460,font=('Segoe UI',10,'bold' if bold else 'normal')).pack(anchor='w',pady=(9,5))
    def checkbox(self,parent,text,bit):
        widget=ttk.Checkbutton(parent,text=text,variable=self.variables[bit],command=lambda:self.toggle(bit))
        widget.pack(anchor='w',pady=(0,8));self.action_widgets.append(widget)
    def apply(self):
        try:
            hp=int(self.hp.get());cooldown=int(self.cooldown.get());dest=int(self.destination.get());spell=self.spell.get().strip();light=int(self.light_level.get())
            if not 1<=hp<=1000000:raise ValueError('HP precisa estar entre 1 e 1000000.')
            if not 1000<=cooldown<=60000:raise ValueError('Intervalo precisa estar entre 1000 e 60000 ms.')
            if not 0<=dest<=63:raise ValueError('Numero da bag precisa estar entre 0 e 63.')
            if not 0<=light<=31:raise ValueError('Intensidade da luz precisa estar entre 0 e 31.')
            if len(spell)>15 or any(not 32<=ord(c)<=126 for c in spell):raise ValueError('A magia aceita ate 15 caracteres ASCII.')
        except ValueError as error:
            messagebox.showerror('Configuracao',str(error),parent=self.root);return False
        with self.session.bridge.lock() as s:
            s.hpThreshold=hp;s.healCooldownMs=cooldown;s.destination=dest;s.healText=spell;s.lightLevel=light
        return True
    def toggle(self,bit):
        if not self.session.connected or self.preview:return
        desired=self.variables[bit].get()
        if not self.apply():self.variables[bit].set(False);return
        s=self.session.bridge.snapshot()
        if desired and bit==HEAL and not s.healText:
            messagebox.showinfo('Healing','Informe a magia antes de ativar.',parent=self.root);self.variables[bit].set(False);return
        if desired and bit==CAVE and not s.waypointCount:
            messagebox.showinfo('Cavebot','Adicione pelo menos um waypoint.',parent=self.root);self.variables[bit].set(False);return
        with self.session.bridge.lock() as state:
            if desired:state.flags|=bit
            else:state.flags&=~bit
    def set_loop(self):
        with self.session.bridge.lock() as s:
            if self.loop.get():s.flags|=LOOP
            else:s.flags&=~LOOP
    def editable_route(self):
        if self.session.bridge.snapshot().flags&CAVE:
            messagebox.showinfo('Waypoints','Desative o cavebot antes de editar a rota.',parent=self.root);return False
        return True
    def add_waypoint(self):
        if self.editable_route():
            with self.session.bridge.lock() as s:s.addSerial+=1
    def clear_waypoints(self):
        if self.editable_route():
            with self.session.bridge.lock() as s:s.clearSerial+=1
    def remove_waypoint(self):
        if not self.editable_route():return
        selected=self.waypoints.curselection()
        if selected:
            with self.session.bridge.lock() as s:s.removeIndex=selected[0];s.removeSerial+=1
    def connect(self):
        if self.preview:return
        if self.session.alive():self.session.stop();return
        self.session=Session(self.pid,self.thread);self.synced=False;self.wp_signature=None;self.session.start()
    def stop_all(self):
        self.session.bridge.disable()
        if self.session.connected:self.session.bridge.status('Parada solicitada','A chamada atual pode terminar. Nenhuma nova automacao sera iniciada.')
    def refresh(self):
        if self.destroyed:return
        s=self.session.bridge.snapshot()
        fault=bool(self.session.controller and self.session.controller.fault)
        enabled=self.session.connected and not self.session.stopping and not self.preview and not fault
        for widget in self.action_widgets:widget.configure(state='normal' if enabled else 'disabled')
        self.connect_button.configure(text=('Aguarde...' if self.session.stopping and self.session.alive() else 'Desconectar' if self.session.alive() else 'Conectar ao Antica'),
                                      state='disabled' if self.preview or self.closing or self.session.stopping and self.session.alive() else 'normal')
        if not self.preview:self.status.set(s.status);self.detail.set(s.detail)
        if self.session.connected and not self.synced:
            self.hp.set(str(s.hpThreshold));self.cooldown.set(str(s.healCooldownMs));self.destination.set(str(s.destination));self.spell.set(s.healText);self.light_level.set(str(s.lightLevel));self.synced=True
        self.light_info.set(s.lightStatus)
        for bit,var in self.variables.items():var.set(bool(s.flags&bit))
        self.loop.set(bool(s.flags&LOOP))
        signature=(s.waypointIndex,tuple(s.waypoints[:s.waypointCount]))
        if signature!=self.wp_signature:
            selected=self.waypoints.curselection();self.waypoints.delete(0,'end')
            for i,p in enumerate(s.waypoints[:s.waypointCount]):
                self.waypoints.insert('end',f"{'>' if i==s.waypointIndex else ' '} {i+1:02}   {p.x}, {p.y}, {p.z}")
            if selected and selected[0]<s.waypointCount:self.waypoints.selection_set(selected[0])
            self.wp_signature=signature
        if self.closing:
            if not self.session.alive():self.destroyed=True;self.root.destroy();return
            self.status.set('Aguardando a chamada atual retornar...');self.detail.set('O painel fechara depois que o controlador terminar.')
        self.root.after(200,self.refresh)
    def close(self):
        self.closing=True;self.session.stop()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid',type=int);parser.add_argument('--thread',type=int)
    parser.add_argument('--preview',action='store_true');parser.add_argument('--self-test',action='store_true')
    args=parser.parse_args();root=tk.Tk();app=App(root,args.pid,args.thread,args.preview or args.self_test)
    passed=False
    if args.self_test:
        root.withdraw()
        def check():
            nonlocal passed
            try:
                assert len(app.tabs.tabs())==5 and len(app.variables)==5
                assert app.session.bridge.snapshot().flags==0 and not app.session.alive()
                assert str(app.apply_button['state'])=='disabled'
                passed=True
                print('PASS: painel externo, cinco abas incluindo Luz, controles e nenhuma conexao/acao automatica')
            finally:app.close()
        root.after(300,check)
    root.mainloop()
    return 0 if not args.self_test or passed else 1


if __name__=='__main__':raise SystemExit(main())
