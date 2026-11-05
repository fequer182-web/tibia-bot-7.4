using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Web.Script.Serialization;

namespace AnticaMonitor {
sealed class AttackReadProfile {
    public ulong GameRva,LocalPlayerOffset,TargetOffset,ControlOffset,TargetVtableRva,ControlVtableRva,AllocationDelta,NameOffset;
    public string OwnerSource;
    public static AttackReadProfile Matching(Profile profile,string directory) {
        string path=Path.Combine(directory,"profiles","validated",profile.Timestamp.ToString("X")+"_"+profile.Hash.ToString("X")+".attack.json");
        if(!File.Exists(path))return null;
        try{return Load(File.ReadAllText(path),profile);}catch{return null;}
    }
    public static AttackReadProfile Load(string json,Profile profile) {
        var root=new JavaScriptSerializer().Deserialize<Dictionary<string,object>>(json);
        if(Convert.ToString(root["schema"])!="cromin-external-target-read-v1"||Convert.ToString(root["status"])!="confirmed_target_read_transition"||Convert.ToBoolean(root["callable"]))throw new Exception("Perfil de alvo incompativel.");
        var build=(Dictionary<string,object>)root["build"];
        if(Profile.Hex(build["timestamp"])!=profile.Timestamp||Profile.Hex(build["sizeOfImage"])!=profile.ImageSize||Profile.Hex(build["textHashFNV1A64"])!=profile.Hash)throw new Exception("Fingerprint de alvo diferente.");
        var p=new AttackReadProfile {GameRva=Profile.Hex(root["gameRva"]),LocalPlayerOffset=Profile.Hex(root["localPlayerOffset"]),TargetOffset=Profile.Hex(root["targetOffset"]),ControlOffset=Profile.Hex(root["controlOffset"]),TargetVtableRva=Profile.Hex(root["targetVtableRva"]),ControlVtableRva=Profile.Hex(root["controlVtableRva"]),AllocationDelta=Profile.Hex(root["allocationDelta"]),NameOffset=Profile.Hex(root["nameOffset"]),OwnerSource=Convert.ToString(root["ownerSource"])};
        if(p.GameRva==0||profile.ImageSize<0x100||p.GameRva>profile.ImageSize-0x100||p.LocalPlayerOffset>0xF8||p.TargetOffset>0xF8||p.ControlOffset>0xF8||p.LocalPlayerOffset%8!=0||p.TargetOffset%8!=0||p.ControlOffset%8!=0||p.TargetOffset==p.ControlOffset||p.TargetOffset==p.LocalPlayerOffset||p.ControlOffset==p.LocalPlayerOffset||p.TargetVtableRva>=profile.ImageSize||p.ControlVtableRva>=profile.ImageSize||p.NameOffset>0x1000||p.AllocationDelta!=16||(p.OwnerSource!="module_direct_field"&&p.OwnerSource!="module_pointer_to_owner")||Convert.ToString(root["nameLayout"])!="msvc_x64_string_sso")throw new Exception("Campos de alvo invalidos.");
        return p;
    }
}
sealed class AttackReading {
    public bool Selected;
    public string Name;
}
static class AttackReader {
    static bool Pointer(ulong value) {return value>=0x10000&&value<=0x7FFFFFFFFFFFUL-0x2000&&value%8==0;}
    static ulong U64(byte[] data,int offset){return BitConverter.ToUInt64(data,offset);}
    static ulong Owner(Func<ulong,int,byte[]> read,ulong module,AttackReadProfile p) {ulong owner=checked(module+p.GameRva);return p.OwnerSource=="module_direct_field"?owner:U64(read(owner,8),0);}
    public static AttackReading Read(Func<ulong,int,byte[]> read,ulong module,Profile profile,AttackReadProfile p,ulong expectedPlayer) {
        if(p==null)throw new Exception("Leitura do alvo pendente para esta build.");
        ulong owner=Owner(read,module,p);if(!Pointer(owner))throw new Exception("Game indisponivel.");
        byte[] game=read(owner,0x100);
        if(U64(game,(int)p.LocalPlayerOffset)!=expectedPlayer)throw new Exception("Personagem mudou.");
        ulong target=U64(game,(int)p.TargetOffset),control=U64(game,(int)p.ControlOffset);
        var result=new AttackReading();
        if(target!=0||control!=0) {
            if(!Pointer(target)||!Pointer(control)||target<=control||target-control!=p.AllocationDelta)throw new Exception("Par de alvo inconsistente.");
            if(U64(read(control,8),0)!=module+p.ControlVtableRva||U64(read(target,8),0)!=module+p.TargetVtableRva)throw new Exception("Tipo de alvo ainda nao validado.");
            byte[] name=read(target+p.NameOffset,32);
            ulong length=U64(name,16),capacity=U64(name,24);
            if(capacity!=15||length==0||length>15||name[(int)length]!=0)throw new Exception("Formato do nome ainda nao validado.");
            string text=new UTF8Encoding(false,true).GetString(name,0,(int)length);
            foreach(char c in text)if(Char.IsControl(c))throw new Exception("Nome invalido.");
            byte[] recheck=read(target+p.NameOffset,32);
            for(int i=0;i<32;i++)if(name[i]!=recheck[i])throw new Exception("Nome mudou durante a leitura.");
            if(U64(read(target,8),0)!=module+p.TargetVtableRva||U64(read(control,8),0)!=module+p.ControlVtableRva)throw new Exception("Objeto mudou durante a leitura.");
            result.Selected=true;result.Name=text;
        }
        byte[] after=read(owner,0x100);
        if(Owner(read,module,p)!=owner||U64(after,(int)p.LocalPlayerOffset)!=expectedPlayer||U64(after,(int)p.TargetOffset)!=target||U64(after,(int)p.ControlOffset)!=control||U64(read(module+profile.Global,8),0)!=expectedPlayer||U64(read(expectedPlayer,8),0)!=module+profile.Vtable)throw new Exception("Alvo ou personagem mudou durante a leitura.");
        return result;
    }
}
}
