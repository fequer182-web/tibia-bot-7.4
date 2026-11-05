using System;
using System.Collections.Generic;
using System.IO;
using System.Web.Script.Serialization;

namespace AnticaMonitor {
static class AttackReaderTests {
    static void Check(bool condition,string message){if(!condition)throw new Exception("FAIL alvo: "+message);}
    static byte[] HexBytes(string text){var data=new byte[text.Length/2];for(int i=0;i<data.Length;i++)data[i]=Convert.ToByte(text.Substring(i*2,2),16);return data;}
    public static void Run(Profile profile) {
        string folder=AppDomain.CurrentDomain.BaseDirectory;
        var attack=AttackReadProfile.Matching(profile,folder);Check(attack!=null,"perfil de leitura carregado");
        var serializer=new JavaScriptSerializer();
        var fixtures=serializer.Deserialize<List<Dictionary<string,object>>>(File.ReadAllText(Path.Combine(folder,"Source","Tests","AlvoCapturas.json")));
        int count=0;
        foreach(var fixture in fixtures) {
            var objects=(System.Collections.IList)fixture["objects"];
            var target=(Dictionary<string,object>)objects[0];var control=(Dictionary<string,object>)objects[1];
            byte[] targetBytes=HexBytes(Convert.ToString(target["BytesHex"]));
            ulong module=BitConverter.ToUInt64(targetBytes,0)-attack.TargetVtableRva,player=0x200000,owner=module+attack.GameRva;
            ulong targetAddress=Profile.Hex(target["Address"]),controlAddress=Profile.Hex(control["Address"]);
            var data=new Dictionary<ulong,byte>();
            Action<ulong,byte[]> put=(address,bytes)=>{for(int i=0;i<bytes.Length;i++)data[address+(ulong)i]=bytes[i];};
            put(owner,new byte[0x100]);put(owner+attack.LocalPlayerOffset,BitConverter.GetBytes(player));put(owner+attack.TargetOffset,BitConverter.GetBytes(targetAddress));put(owner+attack.ControlOffset,BitConverter.GetBytes(controlAddress));
            put(player,BitConverter.GetBytes(module+profile.Vtable));put(targetAddress,targetBytes);put(controlAddress,HexBytes(Convert.ToString(control["BytesHex"])));
            Func<ulong,int,byte[]> read=(address,length)=>{byte[] bytes=new byte[length];for(int i=0;i<length;i++)bytes[i]=data[address+(ulong)i];return bytes;};
            var result=AttackReader.Read(read,module,profile,attack,player);Check(result.Selected&&result.Name==Convert.ToString(fixture["name"]),"nome real de "+fixture["name"]);count++;
            put(owner+attack.TargetOffset,BitConverter.GetBytes(0UL));put(owner+attack.ControlOffset,BitConverter.GetBytes(0UL));Check(!AttackReader.Read(read,module,profile,attack,player).Selected,"cancelamento sem alvo");
            put(owner+attack.TargetOffset,BitConverter.GetBytes(targetAddress));put(owner+attack.ControlOffset,BitConverter.GetBytes(controlAddress));
            int reads=0;bool refused=false;
            try{AttackReader.Read((address,length)=>{if(address==owner&&++reads==2){put(owner+attack.TargetOffset,BitConverter.GetBytes(0UL));}return read(address,length);},module,profile,attack,player);}catch{refused=true;}Check(refused,"troca durante leitura recusada");put(owner+attack.TargetOffset,BitConverter.GetBytes(targetAddress));
            put(targetAddress,BitConverter.GetBytes(0UL));refused=false;try{AttackReader.Read(read,module,profile,attack,player);}catch{refused=true;}Check(refused,"vtable errada recusada");put(targetAddress,BitConverter.GetBytes(module+attack.TargetVtableRva));
            put(targetAddress+attack.NameOffset+24,BitConverter.GetBytes(31UL));refused=false;try{AttackReader.Read(read,module,profile,attack,player);}catch{refused=true;}Check(refused,"nome longo nao validado recusado");
            put(targetAddress+attack.NameOffset+24,BitConverter.GetBytes(15UL));put(owner+attack.LocalPlayerOffset,BitConverter.GetBytes(player+16));refused=false;try{AttackReader.Read(read,module,profile,attack,player);}catch{refused=true;}Check(refused,"relog recusado");
        }
        Check(count==11,"onze capturas reais");
        string metadata=File.ReadAllText(Path.Combine(folder,"profiles","validated",profile.Timestamp.ToString("X")+"_"+profile.Hash.ToString("X")+".attack.json"));
        bool rejected=false;try{AttackReadProfile.Load(metadata.Replace("0x6ACA1FDF","0x6ACA1FDE"),profile);}catch{rejected=true;}Check(rejected,"fingerprint diferente recusada");
        Console.WriteLine("PASS: alvo, 11 capturas reais, cancelamento, corrida, relog, tipo e nome nao validado.");
    }
}
}
