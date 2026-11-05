"""Cromin compacto exclusivo do Ancientia DX."""
import argparse
import tkinter as tk
from tkinter import ttk
from ancientia_funcional import Panel

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid',type=int)
    parser.add_argument('--personagem',default='')
    parser.add_argument('--self-test',action='store_true')
    args=parser.parse_args()
    root=tk.Tk();root.title('Cromin');root.geometry('300x132');root.resizable(False,False)
    detail=tk.Toplevel(root);detail.withdraw()
    panel=Panel(detail,args.pid,args.personagem)
    detail.protocol('WM_DELETE_WINDOW',detail.withdraw)
    # O mesmo controlador atende todas as janelas de configuracao do compacto.
    style=ttk.Style(root);style.layout('Compact.TNotebook.Tab',[])
    panel.tabs.configure(style='Compact.TNotebook')
    def show(index=None):
        if index is not None:panel.tabs.select(index)
        detail.title('Cromin | '+('Conexao' if index is None else ['Cavebot','Targeting','Luz','Loot'][index]))
        detail.deiconify();detail.lift()
    top=ttk.Frame(root);top.pack(fill='x',padx=4,pady=3)
    ttk.Label(top,text='Cromin · Ancientia').pack(side='left')
    ttk.Button(top,text='Parar F8',command=panel.stop,width=9).pack(side='right')
    grid=ttk.Frame(root);grid.pack(fill='x',padx=4)
    for column in range(3):grid.columnconfigure(column,weight=1)
    entries=[('Conectar',lambda:show()),('Cavebot',lambda:show(0)),('Targeting',lambda:show(1)),
             ('Loot',lambda:show(3)),('Luz',lambda:show(2)),('Healing',None)]
    for i,(label,callback) in enumerate(entries):
        ttk.Button(grid,text=label,command=callback,state='normal' if callback else 'disabled').grid(row=i//3,column=i%3,sticky='ew',padx=1,pady=1)
    footer=tk.StringVar(value='Offline · PID: 0')
    ttk.Label(root,textvariable=footer,wraplength=290).pack(anchor='w',padx=5,pady=3)
    def refresh():
        state=panel.sample
        footer.set((f"{state['name']} · PID {panel.pid.get()}" if state else 'Offline · clique Conectar'))
        root.after(500,refresh)
    def close():
        panel.disconnect();root.destroy()
    root.protocol('WM_DELETE_WINDOW',close)
    if args.self_test:
        root.update_idletasks()
        assert len(entries)==6 and not detail.winfo_viewable()
        assert panel.runner is None
        close();print('PASS: compacto Ancientia, seis botoes, configuracao oculta e automacoes desligadas');return 0
    refresh();root.mainloop();return 0

if __name__=='__main__':raise SystemExit(main())
