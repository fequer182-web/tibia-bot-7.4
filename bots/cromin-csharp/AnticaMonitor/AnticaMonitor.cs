using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using System.Drawing;

namespace AnticaMonitor {
sealed class Profile {
    public ulong Timestamp, ImageSize, Hash, Vtable, Global, Hp, MaxHp, X, Y, Z;
    public string Path;
    public ulong LightOffset;
    public bool TorchTestSupported;
    static Dictionary<string,object> Obj(object o) { return (Dictionary<string,object>)o; }
    public static ulong Hex(object o) { string s = Convert.ToString(o); return s.StartsWith("0x") ? Convert.ToUInt64(s.Substring(2),16) : Convert.ToUInt64(s); }
    static ulong Field(Dictionary<string,object> fields,string key) {
        var f=Obj(fields[key]); var offsets=(System.Collections.IList)f["offsets"];
        if (Convert.ToString(f["status"])!="strong_candidate" || offsets.Count!=1) throw new Exception("Campo ambiguo ou pendente: "+key);
        ulong off=Hex(offsets[0]); if(off==0 || off>0x10000) throw new Exception("Offset invalido: "+key); return off;
    }
    public static Profile Load(string path) {
        var root=new JavaScriptSerializer().Deserialize<Dictionary<string,object>>(File.ReadAllText(path));
        if(Convert.ToString(root["schema"])!="otclab-external-v27-port-v1" || Convert.ToString(root["mode"])!="external_read_only") throw new Exception("Perfil externo incompativel.");
        var b=Obj(root["build"]); var lp=Obj(root["localPlayer"]); var pos=Obj(lp["position"]); var fields=Obj(lp["fields"]);
        var p=new Profile {Path=path, Timestamp=Hex(b["timestamp"]), ImageSize=Hex(b["sizeOfImage"]), Hash=Hex(b["textHashFNV1A64"]), Vtable=Hex(lp["vtableRva"]), Global=Hex(lp["directGlobalRva"]), Hp=Field(fields,"health"), MaxHp=Field(fields,"maxHealth"), X=Hex(pos["x"]),Y=Hex(pos["y"]),Z=Hex(pos["z"])};
        if(p.ImageSize<4096 || p.Hash==0 || p.Global==0 || p.Global>p.ImageSize-8 || p.Vtable>=p.ImageSize || p.X>0x10000 || p.Y>0x10000 || p.Z>0x10000) throw new Exception("Perfil sem caminho direto valido.");
        if(root.ContainsKey("light")) {
            var light=Obj(root["light"]); p.LightOffset=Hex(light["bestOffset"]);
            p.TorchTestSupported=p.Timestamp==0x6ACA1FDF && p.Hash==0x8BE8F5F5E4ED7EB2UL && p.ImageSize==0xEDD000 && p.Vtable==0xAE89E8 && p.Global==0xE51528 && p.LightOffset==0xF1 && Convert.ToString(light["status"])=="confirmed_read_transition" && Hex(light["baseline"])==0 && Hex(light["active"])==7;
        }
        return p;
    }
}
sealed class Memory : IDisposable {
    [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,int pid);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool ReadProcessMemory(IntPtr process,IntPtr address,byte[] buffer,UIntPtr size,out UIntPtr read);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool WriteProcessMemory(IntPtr process,IntPtr address,byte[] buffer,UIntPtr size,out UIntPtr written);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    IntPtr handle; public ulong Base;
    public Memory(int pid,bool write=false) {
        using(var process=Process.GetProcessById(pid)) { Base=(ulong)process.MainModule.BaseAddress.ToInt64(); }
        handle=OpenProcess(0x1000|0x0010|(write?0x0028U:0U),false,pid);
        if(handle==IntPtr.Zero) throw new Exception((write?"Escrita externa indisponivel. ":"Leitura externa indisponivel. ")+"Erro Windows: "+Marshal.GetLastWin32Error());
    }
    public byte[] Read(ulong address,int count) {
        byte[] buffer=new byte[count]; UIntPtr got;
        if(!ReadProcessMemory(handle,new IntPtr(checked((long)address)),buffer,(UIntPtr)count,out got) || got.ToUInt64()!=(ulong)count) throw new Exception("Leitura indisponivel. Cliente fechado, leitura bloqueada ou personagem desconectado.");
        return buffer;
    }
    public ulong Pointer(ulong a) {return BitConverter.ToUInt64(Read(a,8),0);}
    public void WriteByte(ulong address,byte value) {
        UIntPtr written;
        if(!WriteProcessMemory(handle,new IntPtr(checked((long)address)),new byte[]{value},(UIntPtr)1,out written) || written.ToUInt64()!=1) throw new Exception("Nao foi possivel escrever o campo de luz. Erro Windows: "+Marshal.GetLastWin32Error());
    }
    public void Verify(Profile p) {
        var dos=Read(Base,64); if(BitConverter.ToUInt16(dos,0)!=0x5A4D) throw new Exception("Executavel invalido.");
        int ntOff=BitConverter.ToInt32(dos,60); if(ntOff<64 || ntOff>0x100000) throw new Exception("Cabecalho invalido.");
        ulong nt=Base+(ulong)ntOff; var hdr=Read(nt,264);
        if(BitConverter.ToUInt32(hdr,0)!=0x4550 || BitConverter.ToUInt16(hdr,4)!=0x8664 || BitConverter.ToUInt16(hdr,24)!=0x20B) throw new Exception("O monitor exige cliente x64.");
        if(BitConverter.ToUInt32(hdr,8)!=p.Timestamp || BitConverter.ToUInt32(hdr,80)!=p.ImageSize) throw new Exception("A versao do cliente mudou. Gere um novo perfil no BuildExplorer.");
        int sections=BitConverter.ToUInt16(hdr,6), opt=BitConverter.ToUInt16(hdr,20); bool found=false;
        if(sections<1 || sections>96) throw new Exception("Secoes invalidas.");
        for(int i=0;i<sections;i++) {
            var sh=Read(nt+24+(ulong)opt+(ulong)i*40,40);
            string name=System.Text.Encoding.ASCII.GetString(sh,0,8).TrimEnd('\0'); if(name!=".text") continue;
            uint rva=BitConverter.ToUInt32(sh,12); ulong size=Math.Max(BitConverter.ToUInt32(sh,8),BitConverter.ToUInt32(sh,16));
            if(rva>=p.ImageSize) throw new Exception("Secao fora do modulo."); size=Math.Min(size,p.ImageSize-rva);
            if(size==0 || size>64*1024*1024) throw new Exception("Secao invalida.");
            ulong hash=14695981039346656037UL;
            for(ulong offset=0;offset<size;) {int len=(int)Math.Min(1024*1024UL,size-offset); hash=Hash(Read(Base+rva+offset,len),hash); offset+=(ulong)len;}
            if(hash!=p.Hash) throw new Exception("Fingerprint diferente. Perfil recusado; refaca a descoberta externa."); found=true; break;
        }
        if(!found) throw new Exception("Secao .text ausente.");
    }
    public static ulong Hash(byte[] bytes,ulong hash) {foreach(byte b in bytes) {hash^=b; hash=unchecked(hash*1099511628211UL);} return hash;}
    public void Dispose() {if(handle!=IntPtr.Zero) {CloseHandle(handle); handle=IntPtr.Zero;}}
}
sealed class Snapshot {
    public ulong Player; public double Hp, MaxHp; public uint X,Y; public byte Z;
    public static Snapshot Read(Func<ulong,int,byte[]> read,ulong module,Profile p) {
        ulong player=BitConverter.ToUInt64(read(module+p.Global,8),0);
        if(player<0x10000 || player>0x7fffffffffff) throw new Exception("Aguardando personagem conectado.");
        if(BitConverter.ToUInt64(read(player,8),0)!=module+p.Vtable) throw new Exception("Personagem nao corresponde ao perfil.");
        var s=new Snapshot {Player=player,Hp=BitConverter.ToDouble(read(player+p.Hp,8),0),MaxHp=BitConverter.ToDouble(read(player+p.MaxHp,8),0),X=BitConverter.ToUInt32(read(player+p.X,4),0),Y=BitConverter.ToUInt32(read(player+p.Y,4),0),Z=read(player+p.Z,1)[0]};
        if(!Valid(s.Hp,s.MaxHp,s.X,s.Y,s.Z)) throw new Exception("Dados invalidos; aguardando leitura consistente.");
        if(BitConverter.ToUInt64(read(module+p.Global,8),0)!=player || BitConverter.ToUInt64(read(player,8),0)!=module+p.Vtable) throw new Exception("Personagem mudou durante a leitura.");
        return s;
    }
    public static bool Valid(double hp,double max,uint x,uint y,byte z) {return !Double.IsNaN(hp) && !Double.IsInfinity(hp) && !Double.IsNaN(max) && !Double.IsInfinity(max) && max>0 && max<=100000000 && hp>=0 && hp<=max && x>0 && x<65535 && y>0 && y<65535 && z<=15;}
}
sealed class Choice {public int Pid;public string Name; public override string ToString(){return Name+" (PID "+Pid+")";}}
sealed class LightTrial {
    readonly Func<ulong,int,byte[]> read; readonly Action<ulong,byte> write; readonly ulong module; readonly Profile profile;
    ulong player; byte original; bool active;
    public LightTrial(Func<ulong,int,byte[]> r,Action<ulong,byte> w,ulong b,Profile p) {read=r;write=w;module=b;profile=p;}
    public void Begin() {
        if(!profile.TorchTestSupported) throw new Exception("Teste de luz indisponivel para este perfil.");
        player=Snapshot.Read(read,module,profile).Player;
        original=read(player+profile.LightOffset,1)[0];
        if(original!=0) throw new Exception("Remova a tocha e espere a luz voltar a zero antes de testar.");
        if(Snapshot.Read(read,module,profile).Player!=player) throw new Exception("Personagem mudou. Teste cancelado.");
        active=true; write(player+profile.LightOffset,7);
        if(read(player+profile.LightOffset,1)[0]!=7) throw new Exception("O cliente nao manteve o valor. Teste interrompido.");
    }
    public string Restore() {
        if(!active) return "Nenhum teste ativo.";
        active=false;
        // Never touch a stale object or overwrite a new light value from the game.
        if(BitConverter.ToUInt64(read(module+profile.Global,8),0)!=player || BitConverter.ToUInt64(read(player,8),0)!=module+profile.Vtable) return "Personagem mudou; restauracao nao aplicada ao objeto antigo.";
        if(read(player+profile.LightOffset,1)[0]!=7) return "O jogo atualizou a luz; seu novo valor foi preservado.";
        write(player+profile.LightOffset,original);
        if(read(player+profile.LightOffset,1)[0]!=original) throw new Exception("Restauracao nao confirmada. Reentre no personagem para redefinir a luz.");
        return "Valor original da luz restaurado.";
    }
}
sealed class MonitorForm : Form {
    ComboBox clients=new ComboBox {DropDownStyle=ComboBoxStyle.DropDownList,Width=310};
    Label status=new Label {AutoSize=false,Width=520,Height=55}, hp=new Label {AutoSize=true}, pos=new Label {AutoSize=true};
    ProgressBar bar=new ProgressBar {Width=510}; NumericUpDown threshold=new NumericUpDown {Minimum=1,Maximum=99,Value=40,Width=65};
    CheckBox alarm=new CheckBox {Text="Alerta sonoro de vida baixa",AutoSize=true};
    Button confirm=new Button {Text="Conferi vida e posicao",Width=200,Enabled=false};
    Button torch=new Button {Text="Testar luz da tocha (5 s)",Width=220,Enabled=false}, restore=new Button {Text="Restaurar luz",Width=150,Enabled=false};
    Label lightStatus=new Label {Width=520,Height=55,Text="Luz: teste experimental; efeito visual ainda nao validado."};
    Memory lightMemory; LightTrial lightTrial; DateTime lightUntil; int connectedPid;
    Timer timer=new Timer {Interval=250}; Profile profile; Memory memory; ulong player; bool validated,low; DateTime lastBeep=DateTime.MinValue; string profilePath;
    public MonitorForm(string path) {
        profilePath=path; Text="Antica Monitor - externo"; ClientSize=new Size(565,550); FormBorderStyle=FormBorderStyle.FixedDialog; MaximizeBox=false; Font=new Font("Segoe UI",10);
        var flow=new FlowLayoutPanel {Dock=DockStyle.Fill,Padding=new Padding(18),FlowDirection=FlowDirection.TopDown,WrapContents=false}; Controls.Add(flow);
        flow.Controls.Add(new Label {Text="Monitor externo | teste de luz opcional",AutoSize=true});
        var row=new FlowLayoutPanel {Width=525,Height=38}; row.Controls.Add(clients); var refresh=new Button {Text="Atualizar",Width=100}; refresh.Click+=(s,e)=>RefreshClients(); row.Controls.Add(refresh); flow.Controls.Add(row);
        var controls=new FlowLayoutPanel {Width=525,Height=38}; var connect=new Button {Text="Conectar",Width=110}; connect.Click+=(s,e)=>Connect(); controls.Controls.Add(connect);
        var disconnect=new Button {Text="Desconectar",Width=110}; disconnect.Click+=(s,e)=>Disconnect(); controls.Controls.Add(disconnect);
        var choose=new Button {Text="Escolher perfil",Width=130}; choose.Click+=(s,e)=>{using(var d=new OpenFileDialog {Filter="Perfil JSON|*.json"}) {if(d.ShowDialog()==DialogResult.OK){Disconnect(); profilePath=d.FileName; status.Text="Perfil selecionado. Clique Conectar.";}}}; controls.Controls.Add(choose); flow.Controls.Add(controls);
        flow.Controls.Add(status); hp.Font=new Font("Segoe UI",20,FontStyle.Bold); flow.Controls.Add(hp); flow.Controls.Add(bar); flow.Controls.Add(pos);
        var alerts=new FlowLayoutPanel {Width=525,Height=35}; alerts.Controls.Add(alarm); alerts.Controls.Add(new Label {Text="Abaixo de %:",AutoSize=true,Padding=new Padding(0,5,0,0)}); alerts.Controls.Add(threshold); flow.Controls.Add(alerts);
        confirm.Click+=(s,e)=>{validated=true; status.Text="Monitorando. Dados conferidos por voce.";torch.Enabled=profile.TorchTestSupported && lightTrial==null;}; flow.Controls.Add(confirm);
        var lights=new FlowLayoutPanel {Width=525,Height=38};lights.Controls.Add(torch);lights.Controls.Add(restore);flow.Controls.Add(lights);flow.Controls.Add(lightStatus);
        torch.Click+=(s,e)=>TestLight();restore.Click+=(s,e)=>RestoreLight();
        timer.Tick+=(s,e)=>Poll(); FormClosed+=(s,e)=>Disconnect(); RefreshClients(); Clear(); status.Text="Abra o cliente, entre no personagem e clique Conectar.";
    }
    void RefreshClients() {
        clients.Items.Clear(); var list=new List<Choice>(); foreach(var p in Process.GetProcesses()){using(p){try {if(p.MainWindowHandle!=IntPtr.Zero) list.Add(new Choice {Pid=p.Id,Name=p.ProcessName});}catch {}}} list.Sort((a,b)=>String.Compare(a.Name,b.Name,StringComparison.OrdinalIgnoreCase));
        foreach(var c in list){clients.Items.Add(c); if(c.Name.IndexOf("Antica",StringComparison.OrdinalIgnoreCase)>=0) clients.SelectedIndex=clients.Items.Count-1;} if(clients.SelectedIndex<0 && clients.Items.Count>0) clients.SelectedIndex=0;
    }
    void Clear(){hp.Text="Vida: --";pos.Text="Posicao: --";bar.Value=0;confirm.Enabled=false;torch.Enabled=false;validated=false;low=false;player=0;}
    void Disconnect(){timer.Stop();RestoreLight();if(memory!=null){memory.Dispose();memory=null;}Clear();status.Text="Desconectado.";}
    void Connect(){Disconnect();try {var c=clients.SelectedItem as Choice; if(c==null) throw new Exception("Selecione o cliente."); profile=Profile.Load(profilePath); status.Text="Conferindo a versao do cliente...";status.Refresh();memory=new Memory(c.Pid);memory.Verify(profile);connectedPid=c.Pid;timer.Start();Poll();}catch(Exception ex){Disconnect();status.Text=ex.Message;}}
    void TestLight() {
        if(memory==null || !validated || lightTrial!=null) return;
        try {lightMemory=new Memory(connectedPid,true);lightMemory.Verify(profile);
            if(lightMemory.Base!=memory.Base || Snapshot.Read(lightMemory.Read,lightMemory.Base,profile).Player!=player) throw new Exception("Cliente ou personagem mudou. Conecte novamente.");
            lightTrial=new LightTrial(lightMemory.Read,lightMemory.WriteByte,lightMemory.Base,profile); lightTrial.Begin();lightUntil=DateTime.UtcNow.AddSeconds(5);torch.Enabled=false;restore.Enabled=true;
            lightStatus.Text="Campo de luz alterado para 7 por 5 s. Confira a claridade no jogo.";
        }catch(Exception ex){RestoreLight();lightStatus.Text=ex.Message+" "+lightStatus.Text;}
    }
    void RestoreLight() {
        if(lightTrial!=null){try{lightStatus.Text=lightTrial.Restore();}catch(Exception ex){lightStatus.Text="Falha ao restaurar: "+ex.Message;}finally{lightTrial=null;}}
        if(lightMemory!=null){lightMemory.Dispose();lightMemory=null;}restore.Enabled=false;torch.Enabled=memory!=null && validated && profile!=null && profile.TorchTestSupported;
    }
    void Poll(){if(memory==null)return;if(lightTrial!=null && DateTime.UtcNow>=lightUntil)RestoreLight();try {var s=Snapshot.Read(memory.Read,memory.Base,profile); if(player!=s.Player){RestoreLight();validated=false;torch.Enabled=false;low=false;player=s.Player;}double percent=100*s.Hp/s.MaxHp;hp.Text=String.Format("Vida: {0:0}/{1:0} ({2:0}%)",s.Hp,s.MaxHp,percent);bar.Value=(int)Math.Max(0,Math.Min(100,percent));pos.Text=String.Format("Posicao: {0}, {1}, {2}",s.X,s.Y,s.Z);confirm.Enabled=true;
        bool next=percent<(double)threshold.Value;if(next && validated && alarm.Checked && (!low || (DateTime.UtcNow-lastBeep).TotalSeconds>=5)){System.Media.SystemSounds.Exclamation.Play();lastBeep=DateTime.UtcNow;}low=next;status.Text=!validated?"Confira vida e posicao no jogo e confirme abaixo.":next?"VIDA BAIXA — monitorando.":"Monitorando. Dados conferidos por voce.";
    }catch(Exception ex){RestoreLight();Clear();status.Text=ex.Message;}}
}
static class Program {
    [STAThread] static int Main(string[] args){
        if(args.Length>0 && args[0]=="--self-test") {try {Tests.Run(args.Length>1?args[1]:"Antica.profile.json");return 0;}catch(Exception e){Console.Error.WriteLine(e);return 1;}}
        Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
        if(args.Length>1 && args[0]=="--preview") {using(var f=new MonitorForm("Antica.profile.json")){f.ShowInTaskbar=false;f.Opacity=0;f.Show();f.PerformLayout();using(var image=new Bitmap(f.Width,f.Height)){f.DrawToBitmap(image,new Rectangle(0,0,f.Width,f.Height));image.Save(args[1]);}}return 0;}
        Application.Run(new MonitorForm(args.Length>0?args[0]:System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"Antica.profile.json")));return 0;
    }
}
static class Tests {
    static void Check(bool ok,string msg){if(!ok)throw new Exception("FAIL: "+msg);}
    public static void Run(string path){
        var p=Profile.Load(path);Check(p.Hp==0x6C0 && p.Global==0xE51528,"perfil Antica");
        Check(Memory.Hash(System.Text.Encoding.ASCII.GetBytes("hello"),14695981039346656037UL)==0xa430d84680aabd0bUL,"FNV1a");
        Check(Snapshot.Valid(109,150,32081,32182,8),"snapshot real"); Check(!Snapshot.Valid(Double.NaN,150,1,1,7),"NaN");Check(!Snapshot.Valid(151,150,1,1,7),"HP acima do maximo");Check(!Snapshot.Valid(1,0,1,1,7),"maximo zero");Check(!Snapshot.Valid(1,150,1,1,16),"andar invalido");
        ulong module=0x100000,player=0x200000;var data=new Dictionary<ulong,byte[]>();data[module+p.Global]=BitConverter.GetBytes(player);data[player]=BitConverter.GetBytes(module+p.Vtable);data[player+p.Hp]=BitConverter.GetBytes(109.0);data[player+p.MaxHp]=BitConverter.GetBytes(150.0);data[player+p.X]=BitConverter.GetBytes((uint)32081);data[player+p.Y]=BitConverter.GetBytes((uint)32182);data[player+p.Z]=new byte[]{8};
        Func<ulong,int,byte[]> read=(a,n)=>data[a];Check(Snapshot.Read(read,module,p).Hp==109,"ponteiro dinamico");
        data[player]=BitConverter.GetBytes(0UL); bool refused=false;try{Snapshot.Read(read,module,p);}catch{refused=true;}Check(refused,"vtable diferente recusada");
        data[player]=BitConverter.GetBytes(module+p.Vtable);int calls=0;refused=false;try{Snapshot.Read((a,n)=>{if(a==module+p.Global && ++calls==2)return BitConverter.GetBytes(player+16);return data[a];},module,p);}catch{refused=true;}Check(refused,"relog durante snapshot recusado");
        string temp=System.IO.Path.GetTempFileName();try {File.WriteAllText(temp,File.ReadAllText(path).Replace("strong_candidate","ambiguous"));refused=false;try{Profile.Load(temp);}catch{refused=true;}Check(refused,"vida ambigua recusada");}finally{File.Delete(temp);}
        Check(p.TorchTestSupported && p.LightOffset==0xF1,"perfil de luz");
        data[player+p.LightOffset]=new byte[]{0};int writes=0;
        Action<ulong,byte> write=(a,v)=>{Check(a==player+p.LightOffset,"escrita limitada ao byte de luz");writes++;data[a]=new byte[]{v};};
        var trial=new LightTrial(read,write,module,p);trial.Begin();Check(writes==1 && data[player+p.LightOffset][0]==7,"luz 7 aplicada uma vez");trial.Restore();Check(writes==2 && data[player+p.LightOffset][0]==0,"luz original restaurada");
        trial=new LightTrial(read,write,module,p);trial.Begin();data[player+p.LightOffset]=new byte[]{3};int before=writes;trial.Restore();Check(writes==before && data[player+p.LightOffset][0]==3,"atualizacao do jogo preservada");
        trial=new LightTrial(read,write,module,p);before=writes;refused=false;try{trial.Begin();}catch{refused=true;}Check(refused && writes==before,"tocha ativa recusa teste");
        data[player+p.LightOffset]=new byte[]{0};trial=new LightTrial(read,write,module,p);trial.Begin();data[module+p.Global]=BitConverter.GetBytes(player+16);before=writes;trial.Restore();Check(writes==before,"sem escrita no objeto antigo apos relog");data[module+p.Global]=BitConverter.GetBytes(player);
        data[player+p.LightOffset]=new byte[]{0};p.TorchTestSupported=false;trial=new LightTrial(read,write,module,p);before=writes;refused=false;try{trial.Begin();}catch{refused=true;}Check(refused && writes==before,"perfil sem suporte recusa escrita");
        Console.WriteLine("PASS: monitor e luz — byte unico, restauracao, atualizacao do jogo, relog e perfil recusado.");
    }
}
}
