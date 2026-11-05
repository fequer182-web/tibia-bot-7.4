using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Windows.Forms;

namespace AnticaMonitor {
sealed class MonitorForm : Form {
    ComboBox clients=new ComboBox {DropDownStyle=ComboBoxStyle.DropDownList,Width=310};
    Label status=new Label {Dock=DockStyle.Bottom,Height=48,Padding=new Padding(8,3,8,2),ForeColor=Color.WhiteSmoke,Font=new Font("Segoe UI",10)};
    Label hp=new Label {Text="Vida: --",Width=310,Height=24,Font=new Font("Segoe UI",10,FontStyle.Bold)},position=new Label {Text="Posicao: --",AutoSize=true};
    Label lightStatus=new Label {Text="LightHack desligado",Width=310,Height=40};
    ProgressBar hpBar=new ProgressBar {Width=310,Height=14};
    CheckBox alarm=new CheckBox {Text="Alerta sonoro",AutoSize=true},light=new CheckBox {Text="LightHack — intensidade 15",AutoSize=true,Enabled=false};
    NumericUpDown threshold=new NumericUpDown {Minimum=1,Maximum=99,Value=40,Width=52};
    Button confirm=new Button {Text="Conferi vida e posicao",Width=185,Height=27,Enabled=false};
    Timer timer=new Timer {Interval=250};
    Label extra=new Label {Width=310,Height=23,Text="Nivel: --  XP: --"};
    bool manualProfile;
    TabControl tabs;bool expanded;int expandedIndex=-1;
    Memory memory,lightMemory; Profile profile; LightTrial lightTrial; string profilePath; int pid; ulong player; bool validated,sync,low; DateTime lastBeep=DateTime.MinValue;
    public MonitorForm(string path) {
        profilePath=path;Text="Cromin";ClientSize=new Size(360,42);BackColor=Color.FromArgb(64,75,76);Font=new Font("Segoe UI",10);FormBorderStyle=FormBorderStyle.FixedSingle;MaximizeBox=false;
        tabs=new TabControl {Location=new Point(4,4),Size=new Size(352,33),Font=new Font("Segoe UI",10),SizeMode=TabSizeMode.Fixed,ItemSize=new Size(110,25)};
        Controls.Add(tabs);Controls.Add(status);
        var healing=Page(tabs,"Healing");healing.Controls.Add(hp);healing.Controls.Add(hpBar);
        healing.Controls.Add(extra);
        var alerts=Row();alerts.Controls.Add(new Label {Text="Abaixo de (%):",AutoSize=true,Padding=new Padding(0,3,0,0)});alerts.Controls.Add(threshold);alerts.Controls.Add(alarm);healing.Controls.Add(alerts);
        healing.Controls.Add(new Label {Text="Vida e alertas; sem cura automatica.",Width=310,Height=24});
        var cavebot=Page(tabs,"Cavebot");cavebot.Controls.Add(position);cavebot.Controls.Add(new Label {Text="Leitura de posicao. Caminhada automatica indisponivel.",Width=310,Height=40});
        var tools=Page(tabs,"Tools");tools.Controls.Add(new Label {Text="Cliente / PID",AutoSize=true});
        tools.Controls.Add(clients);var row=Row();var refresh=Button("Atualizar",74);refresh.Click+=(s,e)=>RefreshClients();row.Controls.Add(refresh);var load=Button("Carregar PID",108);load.Click+=(s,e)=>Connect();row.Controls.Add(load);
        var disconnect=Button("Desconectar",108);disconnect.Click+=(s,e)=>Disconnect();row.Controls.Add(disconnect);tools.Controls.Add(row);
        row=Row();var choose=Button("Perfil...",74);choose.Click+=(s,e)=>ChooseProfile();row.Controls.Add(choose);confirm.Click+=(s,e)=>{validated=true;light.Enabled=profile!=null && profile.TorchTestSupported;status.Text="Dados conferidos. LightHack disponivel em Tools.";};row.Controls.Add(confirm);tools.Controls.Add(row);
        tools.Controls.Add(light);tools.Controls.Add(lightStatus);
        row=Row();var builder=Button("Builder",90);builder.Click+=(s,e)=>OpenBuilder();row.Controls.Add(builder);var onTop=new CheckBox {Text="Sempre por cima",AutoSize=true};onTop.CheckedChanged+=(s,e)=>TopMost=onTop.Checked;row.Controls.Add(onTop);tools.Controls.Add(row);
        light.CheckedChanged+=(s,e)=>{if(sync)return;if(light.Checked)StartLight();else StopLight();};
        timer.Tick+=(s,e)=>Poll();FormClosing+=(s,e)=>Disconnect();RefreshClients();Clear();status.Text="Tools: selecione o cliente e clique Carregar PID.";
        status.Visible=false;
        tabs.MouseUp+=(s,e)=>{for(int i=0;i<tabs.TabCount;i++)if(tabs.GetTabRect(i).Contains(e.Location)){TogglePage(i);break;}};
        tabs.KeyUp+=(s,e)=>{if(e.KeyCode==Keys.Enter || e.KeyCode==Keys.Space || e.KeyCode==Keys.Left || e.KeyCode==Keys.Right)OpenPage(tabs.SelectedIndex);};
    }
    static FlowLayoutPanel Page(TabControl tabs,string name) {var page=new TabPage(name) {BackColor=Color.FromArgb(216,221,222)};tabs.TabPages.Add(page);var flow=new FlowLayoutPanel {Dock=DockStyle.Fill,Padding=new Padding(6),FlowDirection=FlowDirection.TopDown,WrapContents=false,Font=new Font("Segoe UI",10)};page.Controls.Add(flow);return flow;}
    static FlowLayoutPanel Row(){return new FlowLayoutPanel {Width=324,Height=31,WrapContents=false};}
    void TogglePage(int index){if(expanded && expandedIndex==index){expanded=false;expandedIndex=-1;status.Visible=false;tabs.Height=33;ClientSize=new Size(360,42);}else OpenPage(index);}
    public void OpenPage(int index){tabs.SelectedIndex=index;expanded=true;expandedIndex=index;int height=index==0?250:index==1?155:315;ClientSize=new Size(360,height);tabs.Height=height-56;status.Visible=true;}
    public void PreviewTools(){OpenPage(2);}
    static Button Button(string text,int width){return new Button {Text=text,Width=width,Height=27};}
    void RefreshClients(){clients.Items.Clear();var list=new List<Choice>();foreach(var p in Process.GetProcesses()){using(p){try{if(p.MainWindowHandle!=IntPtr.Zero)list.Add(new Choice {Pid=p.Id,Name=p.ProcessName});}catch{}}}list.Sort((a,b)=>String.Compare(a.Name,b.Name,StringComparison.OrdinalIgnoreCase));foreach(var c in list){clients.Items.Add(c);if(c.Name.IndexOf("Antica",StringComparison.OrdinalIgnoreCase)>=0)clients.SelectedIndex=clients.Items.Count-1;}if(clients.SelectedIndex<0 && clients.Items.Count>0)clients.SelectedIndex=0;}
    void Clear(){hp.Text="Vida: --";extra.Text="Nivel: --  XP: --";position.Text="Posicao: --";hpBar.Value=0;confirm.Enabled=false;light.Enabled=false;validated=false;player=0;low=false;}
    void Disconnect(){timer.Stop();StopLight();if(memory!=null){memory.Dispose();memory=null;}Clear();status.Text="Desconectado.";}
    void OpenBuilder(){try{Disconnect();manualProfile=false;string file=System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"Builder","CrominBuilder.exe");Process.Start(new ProcessStartInfo(file) {UseShellExecute=true,WorkingDirectory=System.IO.Path.GetDirectoryName(file)});status.Text="Builder aberto. Ao terminar, carregue o PID novamente.";}catch(Exception ex){status.Text="Nao consegui abrir o Builder: "+ex.Message;}}
    void ChooseProfile(){using(var d=new OpenFileDialog {Filter="Perfil JSON|*.json"}){if(d.ShowDialog()==DialogResult.OK){Disconnect();profilePath=d.FileName;manualProfile=true;status.Text="Perfil selecionado. Carregue o PID novamente.";}}}
    void Connect(){Disconnect();try{var c=clients.SelectedItem as Choice;if(c==null)throw new Exception("Selecione o cliente.");status.Text="Conferindo versao do cliente...";status.Refresh();memory=new Memory(c.Pid);if(manualProfile){profile=Profile.Load(profilePath);memory.Verify(profile);}else profile=Profile.Matching(memory,AppDomain.CurrentDomain.BaseDirectory);pid=c.Pid;timer.Start();Poll();}catch(Exception ex){Disconnect();status.Text=ex.Message;}}
    void StartLight(){
        if(memory==null || !validated){StopLight();return;}
        try{lightMemory=new Memory(pid,true);lightMemory.Verify(profile);if(lightMemory.Base!=memory.Base || Snapshot.Read(lightMemory.Read,lightMemory.Base,profile).Player!=player)throw new Exception("Personagem mudou. Carregue o PID novamente.");
            lightTrial=new LightTrial(lightMemory.Read,lightMemory.WriteByte,lightMemory.Base,profile);lightTrial.Begin();lightStatus.Text="LightHack ligado: 15. Sem limite de tempo.";
        }catch(Exception ex){StopLight();lightStatus.Text="Luz nao ativada: "+ex.Message;}
    }
    void StopLight(){if(lightTrial!=null){try{lightStatus.Text=lightTrial.Restore();}catch(Exception ex){lightStatus.Text="Falha ao restaurar: "+ex.Message;}finally{lightTrial=null;}}
        if(lightMemory!=null){lightMemory.Dispose();lightMemory=null;}sync=true;light.Checked=false;sync=false;
    }
    void Poll(){if(memory==null)return;try{var s=Snapshot.Read(memory.Read,memory.Base,profile);if(player!=s.Player){StopLight();validated=false;light.Enabled=false;low=false;player=s.Player;}
        if(lightTrial!=null){try{lightTrial.Maintain();}catch(Exception ex){StopLight();lightStatus.Text="Luz interrompida: "+ex.Message;}}
        double percent=100*s.Hp/s.MaxHp;hp.Text=String.Format("{0:0} / {1:0} HP  ({2:0}%)",s.Hp,s.MaxHp,percent);hpBar.Value=(int)Math.Max(0,Math.Min(100,percent));position.Text=String.Format("Posicao: {0}, {1}, {2}",s.X,s.Y,s.Z);confirm.Enabled=true;
        extra.Text="Nivel: "+OptionalStat(s.Player,profile.LevelOffset,1000000)+"  XP: "+OptionalStat(s.Player,profile.ExperienceOffset,9007199254740991.0);
        bool next=percent<(double)threshold.Value;if(next && validated && alarm.Checked && (!low || (DateTime.UtcNow-lastBeep).TotalSeconds>=5)){System.Media.SystemSounds.Exclamation.Play();lastBeep=DateTime.UtcNow;}low=next;
        status.Text=!validated?"PID "+pid+" carregado. Confira os dados e confirme em Tools.":next?"Vida baixa — PID "+pid:"Conectado ao PID "+pid;
    }catch(Exception ex){StopLight();Clear();status.Text=ex.Message;}}
    string OptionalStat(ulong expected,ulong offset,double maximum){if(offset==0)return "pendente";try{double value=BitConverter.ToDouble(memory.Read(expected+offset,8),0);if(Double.IsNaN(value) || Double.IsInfinity(value) || value<0 || value>maximum || memory.Pointer(memory.Base+profile.Global)!=expected)return "--";return value.ToString("0");}catch{return "--";}}
}
}
