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
    public const byte Intensity=15;
    readonly Func<ulong,int,byte[]> read; readonly Action<ulong,byte> write; readonly ulong module; readonly Profile profile;
    ulong player; byte original; bool active;
    public LightTrial(Func<ulong,int,byte[]> r,Action<ulong,byte> w,ulong b,Profile p) {read=r;write=w;module=b;profile=p;}
    public void Begin() {
        if(!profile.TorchTestSupported) throw new Exception("Teste de luz indisponivel para este perfil.");
        player=Snapshot.Read(read,module,profile).Player;
        original=read(player+profile.LightOffset,1)[0];
        if(original!=0) throw new Exception("Remova a tocha e espere a luz voltar a zero antes de testar.");
        if(Snapshot.Read(read,module,profile).Player!=player) throw new Exception("Personagem mudou. Teste cancelado.");
        active=true; write(player+profile.LightOffset,Intensity);
        if(read(player+profile.LightOffset,1)[0]!=Intensity) throw new Exception("O cliente nao manteve a luz. Alteracao interrompida.");
    }
    public void Maintain() {
        if(!active)return;
        if(Snapshot.Read(read,module,profile).Player!=player)throw new Exception("Personagem mudou. Ligue a luz novamente apos reconectar.");
        byte now=read(player+profile.LightOffset,1)[0];
        if(now!=Intensity){original=now;write(player+profile.LightOffset,Intensity);if(read(player+profile.LightOffset,1)[0]!=Intensity)throw new Exception("Escrita da luz nao confirmada.");}
    }
    public string Restore() {
        if(!active) return "Nenhum teste ativo.";
        active=false;
        // Never touch a stale object or overwrite a new light value from the game.
        if(BitConverter.ToUInt64(read(module+profile.Global,8),0)!=player || BitConverter.ToUInt64(read(player,8),0)!=module+profile.Vtable) return "Personagem mudou; restauracao nao aplicada ao objeto antigo.";
        if(read(player+profile.LightOffset,1)[0]!=Intensity) return "O jogo atualizou a luz; seu novo valor foi preservado.";
        write(player+profile.LightOffset,original);
        if(read(player+profile.LightOffset,1)[0]!=original) throw new Exception("Restauracao nao confirmada. Reentre no personagem para redefinir a luz.");
        return "Valor original da luz restaurado.";
    }
}
static class Program {
    [STAThread] static int Main(string[] args){
        if(args.Length>0 && args[0]=="--self-test") {try {Tests.Run(args.Length>1?args[1]:"Antica.profile.json");return 0;}catch(Exception e){Console.Error.WriteLine(e);return 1;}}
        Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
        if(args.Length>1 && args[0]=="--preview") {using(var f=new MonitorForm("Antica.profile.json")){f.ShowInTaskbar=false;f.Opacity=0;f.Show();if(args.Length>2){if(args[2]=="healing")f.OpenPage(0);else if(args[2]=="cavebot")f.OpenPage(1);}else f.PreviewTools();f.PerformLayout();using(var image=new Bitmap(f.Width,f.Height)){f.DrawToBitmap(image,new Rectangle(0,0,f.Width,f.Height));image.Save(args[1]);}}return 0;}
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
        var trial=new LightTrial(read,write,module,p);trial.Begin();Check(writes==1 && data[player+p.LightOffset][0]==15,"luz 15 aplicada uma vez");trial.Restore();Check(writes==2 && data[player+p.LightOffset][0]==0,"luz original restaurada");
        trial=new LightTrial(read,write,module,p);trial.Begin();data[player+p.LightOffset]=new byte[]{3};int before=writes;trial.Restore();Check(writes==before && data[player+p.LightOffset][0]==3,"atualizacao do jogo preservada");
        data[player+p.LightOffset]=new byte[]{0};trial=new LightTrial(read,write,module,p);trial.Begin();before=writes;trial.Maintain();Check(writes==before,"sem escrita repetida se intensidade ja 15");data[player+p.LightOffset]=new byte[]{4};trial.Maintain();Check(data[player+p.LightOffset][0]==15 && writes==before+1,"manter luz 15 apos atualizacao");trial.Restore();Check(data[player+p.LightOffset][0]==4,"restaurar ultima luz enviada pelo jogo");
        data[player+p.LightOffset]=new byte[]{3};
        trial=new LightTrial(read,write,module,p);before=writes;refused=false;try{trial.Begin();}catch{refused=true;}Check(refused && writes==before,"tocha ativa recusa teste");
        data[player+p.LightOffset]=new byte[]{0};trial=new LightTrial(read,write,module,p);trial.Begin();data[module+p.Global]=BitConverter.GetBytes(player+16);before=writes;trial.Restore();Check(writes==before,"sem escrita no objeto antigo apos relog");data[module+p.Global]=BitConverter.GetBytes(player);
        data[player+p.LightOffset]=new byte[]{0};p.TorchTestSupported=false;trial=new LightTrial(read,write,module,p);before=writes;refused=false;try{trial.Begin();}catch{refused=true;}Check(refused && writes==before,"perfil sem suporte recusa escrita");
        Console.WriteLine("PASS: monitor e luz — byte unico, restauracao, atualizacao do jogo, relog e perfil recusado.");
    }
}
}
