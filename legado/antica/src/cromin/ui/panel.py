"""Cromin external panel, compact layout based on the supplied Crome screenshots."""
import argparse,json
from pathlib import Path
import tkinter as tk
from tkinter import ttk,messagebox,filedialog
from cromin.core.session import Session
from cromin.core.state import ATTACK,LOOT,CAVE,HEAL,LOOP,LIGHT,FOOD,Position
from cromin.core.paths import DATA
from cromin.core.targeting import parse_names
from cromin.core.loot_policy import LootPolicy,parse_ids

def footer_text(state,pid,online,fault=False):
    look=(f' · ID: {state.lastLookId}' if state.lastLookKind=='item' and state.lastLookId else ' · Look: ?' if state.lastLookKind else '')
    return ('Online' if online else 'Offline')+f' PID: {pid or 0}'+look+(' · erro' if fault else '')

class App:
    def __init__(self,root,pid=None,thread=None,preview=False):
        self.root=root;self.pid=pid;self.thread=thread;self.preview=preview
        self.session=Session(pid,thread);self.closing=False;self.destroyed=False
        self.synced=False;self.wp_signature=None;self.action_widgets=[]
        self.selected=None;self.wide=False;self.dark=False;self.drag=None
        root.title('Cromin | Compacto');root.configure(bg='#f0f5fa')
        root.overrideredirect(True);root.resizable(False,False)
        self.style=ttk.Style(root);self.style.theme_use('clam')
        self.status=tk.StringVar(value='Previa: sem conexao' if preview else 'Desconectado')
        self.detail=tk.StringVar(value='Nenhuma acao sera executada.' if preview else 'Clique no rodape para conectar.')
        self.footer=tk.StringVar(value='Previa · PID: —' if preview else 'Offline  PID: 0')
        self.variables={bit:tk.BooleanVar(value=False) for bit in (ATTACK,LOOT,CAVE,HEAL,LIGHT,FOOD)}
        self.hp=tk.StringVar(value='80');self.cooldown=tk.StringVar(value='2000');self.spell=tk.StringVar()
        self.target_names=tk.StringVar(value='Rat');self.target_any=tk.BooleanVar(value=False);self.target_follow=tk.BooleanVar(value=True)
        self.corpse_ids=tk.StringVar(value='3994');self.loot_ids=tk.StringVar(value='3031');self.drop_ids=tk.StringVar(value='')
        self.destination=tk.StringVar(value='0')
        self.food_ids=tk.StringVar(value='3577')
        self.food_info=tk.StringVar(value='Auto Food desligado')
        self.light_level=tk.StringVar(value='31');self.light_info=tk.StringVar(value='Desligada')
        self.topmost=tk.BooleanVar(value=True)
        self.header=ttk.Frame(root);self.header.pack(fill='x',padx=4,pady=(3,2))
        title=ttk.Label(self.header,text='Cromin');title.pack(side='left',fill='x',expand=True)
        title.bind('<ButtonPress-1>',self.start_drag);title.bind('<B1-Motion>',self.move_drag)
        self.toolbar={}
        for text,command in [('O',self.open_profile),('S',self.save_profile),('P',self.pin),('+',self.resize_panel),('D',self.theme),('-',self.minimize),('X',self.close)]:
            button=ttk.Button(self.header,text=text,width=2,command=command)
            button.pack(side='left',padx=(0,2));self.toolbar[text]=button
        self.menu=ttk.Frame(root);self.menu.pack(fill='x',padx=4)
        self.menu_buttons={};self.pages={};self.scrollers=[]
        self.body=ttk.Frame(root)
        for index,name in enumerate(('Healing','Cavebot','Targeting','Loot','Hotkeys','Tools')):
            button=ttk.Button(self.menu,text=name,command=lambda n=name:self.select_panel(n))
            button.grid(row=index//3,column=index%3,sticky='ew',padx=(0,3 if index%3<2 else 0),pady=(0,3))
            self.menu_buttons[name]=button;self.menu.columnconfigure(index%3,weight=1,uniform='menu')
            host=ttk.Frame(self.body)
            canvas=tk.Canvas(host,highlightthickness=0)
            vertical=ttk.Scrollbar(host,orient='vertical',command=canvas.yview)
            horizontal=ttk.Scrollbar(host,orient='horizontal',command=canvas.xview)
            vertical.pack(side='right',fill='y');horizontal.pack(side='bottom',fill='x')
            canvas.pack(side='left',fill='both',expand=True)
            canvas.configure(yscrollcommand=vertical.set,xscrollcommand=horizontal.set)
            page=ttk.Frame(canvas,padding=4);canvas_window=canvas.create_window((0,0),window=page,anchor='nw',width=330)
            if name=='Tools':
                vertical.pack_forget();horizontal.pack_forget();canvas.itemconfigure(canvas_window,width=240)
            page.bind('<Configure>',lambda event,c=canvas:c.configure(scrollregion=c.bbox('all')))
            canvas.bind('<MouseWheel>',lambda event,c=canvas:c.yview_scroll(-int(event.delta/120),'units'))
            self.pages[name]=host;self.scrollers.append(canvas)
            heading=ttk.Label(page,text=name,style='Heading.TLabel',padding=4);heading.pack(fill='x',pady=(0,5))
            if name=='Healing':
                self.checkbox(page,'Healing por magia',HEAL)
                self.field(page,'HP absoluto ≤',self.hp,8)
                self.field(page,'Magia',self.spell,23)
                self.field(page,'Intervalo (ms)',self.cooldown,8)
                self.note(page,'HP absoluto, sem porcentagem. Retorno da fala nao confirma a magia.')
            elif name=='Cavebot':
                self.checkbox(page,'Ativar cavebot',CAVE)
                self.waypoints=tk.Listbox(page,height=5,exportselection=False,relief='flat',highlightthickness=1,font=('Consolas',8))
                self.waypoints.pack(fill='x',pady=4)
                row=ttk.Frame(page);row.pack(fill='x')
                for text,command in [('Gravar',self.add_waypoint),('Remover',self.remove_waypoint),('Limpar',self.clear_waypoints)]:
                    button=ttk.Button(row,text=text,command=command);button.pack(side='left',padx=(0,3));self.action_widgets.append(button)
                self.note(page,'Caminhada no mesmo andar. Grave pontos com o cavebot desligado.')
            elif name=='Targeting':
                self.checkbox(page,'Ativar Target',ATTACK)
                self.field(page,'Nomes',self.target_names,24)
                ttk.Checkbutton(page,text='Qualquer monstro',variable=self.target_any).pack(anchor='w',pady=3)
                ttk.Checkbutton(page,text='Follow (perseguir o alvo)',variable=self.target_follow).pack(anchor='w',pady=3)
                self.note(page,'Separe nomes por virgula: Rat, Bat, Snake. Correspondencia exata, sem diferenca de maiusculas. Aplica ao clicar Aplicar.')
                self.note(page,'Escolhe o mais proximo no mesmo andar. Jogadores e NPCs ficam fora. Loot usa os IDs de corpos configurados na secao Loot.')
            elif name=='Loot':
                self.checkbox(page,'Ativar Loot',LOOT)
                self.field(page,'Bag de destino',self.destination,8)
                canvas.itemconfigure(canvas_window,width=240)
                for label,var in [('IDs dos corpos',self.corpse_ids),('IDs para recolher',self.loot_ids),('IDs para Drop',self.drop_ids)]:
                    ttk.Label(page,text=label).pack(anchor='w',pady=(4,0))
                    ttk.Entry(page,textvariable=var,width=30).pack(anchor='w',fill='x')
                ttk.Label(page,text='IDs por virgula. Abre bags 2853 no corpo. Drop tira do corpo e joga no seu SQM.',wraplength=220,font=('Segoe UI',7)).pack(anchor='w',pady=3)
            elif name=='Hotkeys':
                self.note(page,'F8 — parar automacoes',True)
                self.note(page,'Funciona quando esta janela tem foco. O botao Parar tambem fica disponivel na barra inferior.')
                self.note(page,'Atalhos globais ainda nao implementados.')
            else:
                self.checkbox(page,'Light Hack',LIGHT)
                self.checkbox(page,'Auto Food',FOOD)
                self.field(page,'IDs de food',self.food_ids,14)
                self.note(page,'3577: coxinha. IDs separados por virgula.')
        self.runtime=tk.Toplevel(root);self.runtime.withdraw();self.runtime.title('Cromin | Conexao')
        self.runtime.geometry('360x230');self.runtime.protocol('WM_DELETE_WINDOW',self.runtime.withdraw)
        runtime=ttk.Frame(self.runtime,padding=10);runtime.pack(fill='both',expand=True)
        self.connect_button=ttk.Button(runtime,text='Conectar ao Antica',command=self.connect)
        self.connect_button.pack(fill='x',pady=4)
        ttk.Label(runtime,textvariable=self.status,wraplength=330).pack(anchor='w')
        ttk.Label(runtime,textvariable=self.detail,wraplength=330).pack(anchor='w',pady=4)
        ttk.Label(runtime,textvariable=self.light_info,wraplength=330).pack(anchor='w',pady=2)
        ttk.Label(runtime,textvariable=self.food_info,wraplength=330).pack(anchor='w',pady=2)
        self.look_info=tk.StringVar(value='Look aguardando conexao')
        ttk.Label(runtime,textvariable=self.look_info,wraplength=330).pack(anchor='w',pady=2)
        self.commands=ttk.Frame(root)
        self.apply_button=ttk.Button(self.commands,text='Aplicar',command=self.apply);self.apply_button.pack(side='left',fill='x',expand=True,padx=(0,3))
        self.action_widgets.append(self.apply_button)
        self.stop_button=ttk.Button(self.commands,text='Parar · F8',command=self.stop_all);self.stop_button.pack(side='left',fill='x',expand=True)
        self.footer_widget=ttk.Label(root,textvariable=self.footer,cursor='hand2')
        self.footer_widget.pack(side='bottom',fill='x',padx=4,pady=2)
        self.footer_widget.bind('<Button-1>',lambda event:self.show_runtime())
        self.footer_widget.bind('<Button-3>',lambda event:self.copy_look_id())
        root.bind('<F8>',lambda event:self.stop_all());root.protocol('WM_DELETE_WINDOW',self.close)
        root.bind('<Map>',self.restore_borderless,add='+')
        self.apply_theme();self.layout();root.attributes('-topmost',True);self.refresh()
    def copy_look_id(self):
        s=self.session.bridge.snapshot()
        if s.lastLookKind=='item' and s.lastLookId:
            self.root.clipboard_clear();self.root.clipboard_append(str(s.lastLookId))
    def show_runtime(self):
        self.runtime.deiconify();self.runtime.attributes('-topmost',self.topmost.get());self.runtime.lift()
    def note(self,parent,text,bold=False):
        ttk.Label(parent,text=text,wraplength=300,font=('Segoe UI',7,'bold' if bold else 'normal')).pack(anchor='w',pady=3)
    def field(self,parent,label,variable,width):
        row=ttk.Frame(parent);row.pack(fill='x',pady=2)
        ttk.Label(row,text=label,width=18).pack(side='left')
        ttk.Entry(row,textvariable=variable,width=width).pack(side='left')
    def checkbox(self,parent,text,bit):
        widget=ttk.Checkbutton(parent,text=text,variable=self.variables[bit],command=lambda:self.toggle(bit))
        widget.pack(anchor='w',pady=2);self.action_widgets.append(widget)
    def start_drag(self,event):self.drag=(event.x_root-self.root.winfo_x(),event.y_root-self.root.winfo_y())
    def move_drag(self,event):
        if self.drag:self.root.geometry(f'+{event.x_root-self.drag[0]}+{event.y_root-self.drag[1]}')
    def select_panel(self,name):
        self.selected=None if self.selected==name else name
        for host in self.pages.values():host.pack_forget()
        if self.selected:self.pages[self.selected].pack(fill='both',expand=True)
        self.layout();self.apply_theme()
    def layout(self):
        self.body.pack_forget();self.commands.pack_forget()
        width=520 if self.wide else 260
        height=(260 if self.selected=='Tools' else (460 if self.wide else 290)) if self.selected else 100
        if self.selected:
            self.commands.pack(side='bottom',fill='x',padx=4,pady=2,after=self.footer_widget)
            self.body.pack(fill='both',expand=True,padx=4,pady=(1,2),after=self.commands)
        self.root.geometry(f'{width}x{height}')
    def resize_panel(self):self.wide=not self.wide;self.layout();self.apply_theme()
    def pin(self):self.topmost.set(not self.topmost.get());self.root.attributes('-topmost',self.topmost.get());self.apply_theme()
    def theme(self):self.dark=not self.dark;self.apply_theme()
    def apply_theme(self):
        bg,fg,field,border=('#18212b','#e4edf5','#222f3d','#415269') if self.dark else ('#f0f5fa','#273a4f','#f7fbff','#b9cde2')
        self.root.configure(bg=bg)
        self.style.configure('.',font=('Segoe UI',7),background=bg,foreground=fg)
        self.style.configure('TFrame',background=bg)
        self.style.configure('TLabel',background=bg,foreground=fg)
        self.style.configure('TButton',padding=(2,2),background=field,foreground=fg,bordercolor=border,lightcolor=field,darkcolor=field)
        self.style.map('TButton',background=[('active','#d7e9fb')],foreground=[('disabled','#8797a8')])
        self.style.configure('Active.TButton',background='#0875ed',foreground='white')
        self.style.map('Active.TButton',background=[('active','#086bd3')],foreground=[('active','white')])
        self.style.configure('Heading.TLabel',background='#e2f1ff' if not self.dark else '#263c53',foreground=fg,font=('Segoe UI',7,'bold'))
        self.style.configure('TCheckbutton',background=bg,foreground=fg,padding=1)
        self.style.configure('TEntry',fieldbackground=field,foreground=fg,bordercolor=border,padding=1)
        self.style.configure('TNotebook',background=bg,borderwidth=0)
        self.style.configure('TNotebook.Tab',padding=(6,2),background=field,foreground=fg)
        for name,button in self.menu_buttons.items():button.configure(style='Active.TButton' if name==self.selected else 'TButton')
        self.toolbar['P'].configure(style='Active.TButton' if self.topmost.get() else 'TButton')
        self.toolbar['+'].configure(text='−' if self.wide else '+')
        for canvas in self.scrollers:canvas.configure(bg=bg)
        self.waypoints.configure(bg=field,fg=fg,selectbackground='#0875ed',selectforeground='white',highlightbackground=border)
    def minimize(self):self.root.overrideredirect(False);self.root.iconify()
    def restore_borderless(self,event):
        if event.widget==self.root and self.root.state()=='normal':self.root.after_idle(lambda:self.root.overrideredirect(True) if not self.destroyed else None)
    def profile_data(self):
        s=self.session.bridge.snapshot()
        return {'food_ids':parse_ids(s.foodIds),**LootPolicy.from_state(s).settings(),'hp':s.hpThreshold,'cooldown':s.healCooldownMs,'destination':s.destination,'spell':s.healText,'light':31,'target_follow':s.targetFollow,'target_names':parse_names(s.targetName),'target_any':s.targetAny,
                'loop':True,'waypoints':[{'x':p.x,'y':p.y,'z':p.z} for p in s.waypoints[:s.waypointCount]]}
    def open_profile(self):
        if self.preview:return
        if self.session.alive():messagebox.showinfo('Configuracao','Desconecte antes de carregar outra configuracao.',parent=self.root);return
        path=filedialog.askopenfilename(parent=self.root,title='Configuracao do Cromin',filetypes=[('JSON','*.json')])
        if not path:return
        try:
            from cromin.core.profiles import validate_profile
            data=validate_profile(json.loads(Path(path).read_text(encoding='utf-8-sig')))
            target=DATA/'Cromin_Externo.settings.json';temporary=target.with_suffix('.tmp')
            temporary.write_text(json.dumps(data,ensure_ascii=False,indent=2),encoding='utf-8');temporary.replace(target)
            self.hp.set(str(data['hp']));self.cooldown.set(str(data['cooldown']));self.destination.set(str(data['destination']))
            self.food_ids.set(', '.join(map(str,data['food_ids'])))
            self.set_loot_fields(LootPolicy.create(data['corpse_ids'],data['loot_ids'],data['drop_ids'],data['drop_scope']))
            self.spell.set(data['spell']);self.light_level.set(str(data['light']));self.target_names.set(', '.join(data['target_names']));self.target_any.set(data['target_any']);self.target_follow.set(data['target_follow'])
            with self.session.bridge.lock() as s:
                s.foodIds=', '.join(map(str,data['food_ids']))
                self.store_loot(s,LootPolicy.create(data['corpse_ids'],data['loot_ids'],data['drop_ids'],data['drop_scope']))
                s.hpThreshold=data['hp'];s.healCooldownMs=data['cooldown'];s.destination=data['destination']
                s.healText=data['spell'];s.lightLevel=data['light'];s.targetName=', '.join(data['target_names']);s.targetAny=data['target_any'];s.targetFollow=data['target_follow'];s.flags=0
                s.waypointCount=len(data['waypoints']);s.waypointIndex=0
                for i,p in enumerate(data['waypoints']):s.waypoints[i]=Position(p['x'],p['y'],p['z'])
            self.session.bridge.status('Configuracao carregada. Conecte ao Antica.')
        except (ValueError,OSError,TypeError) as error:messagebox.showerror('Configuracao',str(error),parent=self.root)
    def save_profile(self):
        if self.preview or not self.apply():return
        path=filedialog.asksaveasfilename(parent=self.root,title='Salvar configuracao Cromin',defaultextension='.json',filetypes=[('JSON','*.json')])
        if path:
            try:Path(path).write_text(json.dumps(self.profile_data(),ensure_ascii=False,indent=2),encoding='utf-8')
            except OSError as error:messagebox.showerror('Configuracao',str(error),parent=self.root)

    @staticmethod
    def store_loot(s,policy):
        s.corpseIds=', '.join(map(str,policy.corpses));s.lootIds=', '.join(map(str,policy.take));s.dropIds=', '.join(map(str,policy.drop));s.dropScope=policy.drop_scope
    def set_loot_fields(self,policy):
        self.corpse_ids.set(', '.join(map(str,policy.corpses)));self.loot_ids.set(', '.join(map(str,policy.take)));self.drop_ids.set(', '.join(map(str,policy.drop)))
    def apply(self):
        try:
            hp=int(self.hp.get());cooldown=int(self.cooldown.get());dest=int(self.destination.get());spell=self.spell.get().strip();light=31
            policy=LootPolicy.create(self.corpse_ids.get(),self.loot_ids.get(),self.drop_ids.get(),'corpse')
            foods=parse_ids(self.food_ids.get())
            names=parse_names(self.target_names.get())
            if not names and not self.target_any.get():raise ValueError('Informe nomes ou marque Qualquer monstro.')
            if not 1<=hp<=1000000:raise ValueError('HP precisa estar entre 1 e 1000000.')
            if not 1000<=cooldown<=60000:raise ValueError('Intervalo precisa estar entre 1000 e 60000 ms.')
            if not 0<=dest<=63:raise ValueError('Numero da bag precisa estar entre 0 e 63.')
            if not 0<=light<=31:raise ValueError('Intensidade da luz precisa estar entre 0 e 31.')
            if len(spell)>15 or any(not 32<=ord(c)<=126 for c in spell):raise ValueError('A magia aceita ate 15 caracteres ASCII.')
        except ValueError as error:
            messagebox.showerror('Configuracao',str(error),parent=self.root);return False
        with self.session.bridge.lock() as s:
            s.foodIds=', '.join(map(str,foods))
            self.store_loot(s,policy)
            s.hpThreshold=hp;s.healCooldownMs=cooldown;s.destination=dest;s.healText=spell;s.lightLevel=light;s.targetName=', '.join(names);s.targetAny=self.target_any.get();s.targetFollow=self.target_follow.get()
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
            if desired:
                state.flags|=bit
            else:state.flags&=~bit
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
            self.food_ids.set(s.foodIds)
            self.set_loot_fields(LootPolicy.from_state(s))
            self.hp.set(str(s.hpThreshold));self.cooldown.set(str(s.healCooldownMs));self.destination.set(str(s.destination));self.spell.set(s.healText);self.light_level.set(str(s.lightLevel));self.target_names.set(s.targetName);self.target_any.set(s.targetAny);self.target_follow.set(s.targetFollow);self.synced=True
        if not self.preview:
            self.footer.set(footer_text(s,self.session.pid,self.session.connected,fault))
        self.food_info.set(s.foodStatus);self.light_info.set(s.lightStatus);self.look_info.set(s.lookStatus)
        for bit,var in self.variables.items():var.set(bool(s.flags&bit))
        signature=(s.waypointIndex,tuple(s.waypoints[:s.waypointCount]))
        if signature!=self.wp_signature:
            selected=self.waypoints.curselection();self.waypoints.delete(0,'end')
            for i,p in enumerate(s.waypoints[:s.waypointCount]):
                self.waypoints.insert('end',f"{'>' if i==s.waypointIndex else ' '} {i+1:02}   {p.x}, {p.y}, {p.z}")
            if selected and selected[0]<s.waypointCount:self.waypoints.selection_set(selected[0])
            self.wp_signature=signature
        if self.closing:
            if not self.session.alive():self.destroyed=True;self.root.destroy();return
            self.footer.set('Aguardando a chamada atual...')
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
                root.deiconify();root.update()
                assert len(app.menu_buttons)==6 and app.selected is None
                root.update();assert (root.winfo_width(),root.winfo_height())==(260,100)
                for name in app.menu_buttons:
                    app.select_panel(name);root.update()
                    assert app.selected==name and root.winfo_height()==(260 if name=='Tools' else 290)
                    app.select_panel(name);root.update();assert app.selected is None and root.winfo_height()==100
                app.select_panel('Cavebot');app.resize_panel();root.update()
                assert (root.winfo_width(),root.winfo_height())==(520,460)
                app.theme();assert app.dark;app.theme();assert not app.dark
                app.pin();assert not app.topmost.get();app.pin()
                app.target_names.set('Rat, BAT, rat');app.target_any.set(True);assert app.apply()
                assert app.session.bridge.snapshot().targetName=='Rat, BAT' and app.session.bridge.snapshot().targetAny
                assert app.profile_data()['target_names']==['Rat','BAT']
                assert app.profile_data()['light']==31 and app.session.bridge.snapshot().targetFollow
                app.target_follow.set(False);assert app.apply();assert not app.session.bridge.snapshot().targetFollow
                app.corpse_ids.set('3994, 4242');app.loot_ids.set('3031, 3552');app.drop_ids.set('3492');assert app.apply()
                assert app.profile_data()['corpse_ids']==[3994,4242] and app.profile_data()['drop_ids']==[3492]
                assert app.session.bridge.snapshot().lootIds=='3031, 3552'
                assert len(app.pages['Tools'].winfo_children())==3
                assert not hasattr(app,'loop_control')
                app.food_ids.set('3577, 3582');assert app.apply();assert app.profile_data()['food_ids']==[3577,3582]
                app.show_runtime();root.update();assert app.runtime.winfo_ismapped();app.runtime.withdraw()
                assert app.session.bridge.snapshot().flags==0 and not app.session.alive()
                assert str(app.apply_button['state'])=='disabled'
                passed=True
                print('PASS: 260x100, seis paineis expansivos, modo amplo, temas, fixar e nenhuma conexao/acao automatica')
            finally:app.close()
        root.after(300,check)
    root.mainloop()
    return 0 if not args.self_test or passed else 1

if __name__=='__main__':raise SystemExit(main())
