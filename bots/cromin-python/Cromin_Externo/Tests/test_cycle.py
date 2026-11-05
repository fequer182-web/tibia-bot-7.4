from pathlib import Path
import copy,struct,sys
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root))
from world_read import WorldReader,RatDataError
from open_and_loot import open_and_collect
from rat_cycle_once import cycle,fresh_corpse,tracked_observation

class Memory:
    def __init__(self):self.data={}
    def alloc(self,a,n):self.data.update({a+i:0 for i in range(n)})
    def put(self,a,fmt,*v):self.data.update({a+i:b for i,b in enumerate(struct.pack(fmt,*v))})
    def exact(self,a,n):return bytes(self.data[a+i] for i in range(n))
    def alive(self):pass

BASE=0x140000000
def make_world(x=100,y=101):
    m=Memory();header=0x100000;node=0x110000;tile=0x120000;item=0x130000;vec=0x140000;player=0x150000
    for a,n in ((header,0x38),(node,0x4028),(tile,0x70),(item,0x50),(vec,32),(player,0x80)):
        m.alloc(a,n)
    m.put(BASE+0xE52E30+7*16,'<QQ',header,1);m.put(header+8,'Q',node);m.put(header+0x19,'B',1)
    m.put(node,'<QQQ',header,header,header);m.put(node+0x20,'I',(y>>5)*2048+(x>>5))
    slot=node+0x28+(((y&31)*32+(x&31))*16);m.put(slot,'<QQ',tile,0x99999)
    m.put(tile,'Q',BASE+0xAF6848);m.put(tile+0x50,'<QQQ',vec,vec+16,vec+32)
    m.put(vec,'<QQ',item,0x22222);m.put(item,'Q',BASE+0xAE9BC0);m.put(item+0x20,'<IIH',x,y,7);m.put(item+0x40,'H',3994)
    m.put(BASE+0xE52A48,'Q',player);m.put(player,'Q',BASE+0xAE99E0);m.put(player+0x30,'<IIH',x,y-1,7)
    return m,WorldReader(m,BASE),locals()

for x,y in ((100,101),(31,31),(32,32),(65534,65534)):
    m,w,a=make_world(x,y);p={'x':x,'y':y,'z':7}
    assert w.corpses_at(p)[0]['object']==hex(a['item'])
    assert w.corpses_at(dict(p,x=x-1))==[]
print('PASS: Tile/Things, limites de bloco 31/32 e coordenadas maximas')
for bad in ('position','vtable','vector','control'):
    m,w,a=make_world();p={'x':100,'y':101,'z':7}
    if bad=='position':m.put(a['item']+0x20,'I',99)
    if bad=='vtable':m.put(a['tile'],'Q',0)
    if bad=='vector':m.put(a['tile']+0x58,'Q',a['vec']+15)
    if bad=='control':m.put(a['vec']+8,'Q',0)
    try:w.corpses_at(p)
    except ValueError:pass
    else:raise AssertionError(bad)
print('PASS: rejeita corpo fora do tile, VTable, vetor e controle invalidos')

# Mapa de criaturas com um Rat adjacente; rastreia ID e posicao atuais.
m,w,a=make_world();h,n,r=0x200000,0x210000,0x220000
for addr,size in ((h,0x38),(n,0x38),(r,0x70)):m.alloc(addr,size)
m.put(BASE+0xE52F30,'<QQ',h,1);m.put(h+8,'Q',n);m.put(h+0x19,'B',1)
m.put(n,'<QQQ',h,h,h);m.put(n+0x20,'I',42);m.put(n+0x28,'<QQ',r,0x44444)
m.put(r,'Q',BASE+0xAF61D8);m.put(r+0x20,'<IIH',100,101,7);m.put(r+0x40,'I',42)
m.put(r+0x48,'4s',b'Rat\0');m.put(r+0x58,'<QQ',3,15);m.put(r+0x69,'B',100)
assert w.nearby_rats()[0]['id']==42
# Um segundo Rat fora do mapa nao pode bloquear o Rat valido adjacente.
n2,r2=0x230000,0x240000
m.alloc(n2,0x38);m.alloc(r2,0x70)
m.put(n+0x10,'Q',n2);m.put(n2,'<QQQ',h,n,h);m.put(n2+0x20,'I',43);m.put(n2+0x28,'<QQ',r2,0x55555)
m.put(BASE+0xE52F38,'Q',2)
for index,value in enumerate(m.exact(r,0x70)):m.put(r2+index,'B',value)
m.put(r2+0x40,'I',43);m.put(r2+0x20,'<IIH',65535,65535,255)
assert [v['id'] for v in w.nearby_rats()]==[42]
assert w.rat_rejected_count==1 and w.rat_diagnostics[0]['position']=={'x':65535,'y':65535,'z':255}
assert w.rat_diagnostics[0]['hp']==100 and w.rat_diagnostics[0]['position_raw_hex']
try:w.rat(43)
except RatDataError as error:assert error.details['id']==43
else:raise AssertionError('Invalid tracked Rat treated as missing/dead')
m.put(r2+0x20,'<IIH',100,101,7);m.put(r2+0x69,'B',255)
assert [v['id'] for v in w.nearby_rats()]==[42] and w.rat_diagnostics[0]['hp']==255
m.put(n+0x10,'Q',h);m.put(BASE+0xE52F38,'Q',1)
assert w.nearby_rats()[0]['id']==42 and w.rat_diagnostics==[]
# Selecao de andar aceita distancia; a busca adjacente continua separada.
m.put(r+0x20,'<IIH',105,101,7)
assert w.nearby_rats()==[] and w.rats_on_floor()[0]['id']==42
m.put(r+0x20,'<IIH',105,101,8);assert w.rats_on_floor()==[]
m.put(r+0x20,'<IIH',100,101,7)
m.put(n+0x10,'Q',n2);m.put(BASE+0xE52F38,'Q',2)
m.put(r2+0x20,'<IIH',104,101,7);m.put(r2+0x69,'B',100)
assert [v['id'] for v in w.rats_on_floor()]==[42,43]
m.put(r+0x20,'<IIH',106,101,7);assert [v['id'] for v in w.rats_on_floor()]==[43,42]
m.put(r2+0x20,'<IIH',104,101,8);assert [v['id'] for v in w.rats_on_floor()]==[42]
m.put(n+0x10,'Q',h);m.put(BASE+0xE52F38,'Q',1);m.put(r+0x20,'<IIH',100,101,7)
print('PASS: mesmo andar aceita Rat distante, exclui outro andar e ordena por proximidade')
print('PASS: Rat invalido nao bloqueia alvo valido; diagnostico detalhado; alvo rastreado continua estrito')
m.put(r+0x69,'B',0);assert w.nearby_rats()==[] and w.rat(42)['hp']==0
m.put(h+8,'Q',h);assert w.rat(42) is None
print('PASS: mapa de criaturas, Rat vivo, vida zero e ID removido')

POSITION={'x':100,'y':101,'z':7}
BODY={'object':'0x123456','control':'0x123450','tile':'0x999999','tile_slot':0,'id':3994,'position':POSITION}
def container(cid,bodyid,gold):
    return {'container_id':cid,'address':hex(0x300000+cid*0x1000),'bag_item':{'id':bodyid,'address':hex(0x400000+cid*0x1000)},
            'capacity':8,'item_count':1 if gold else 0,'items':[{'id':3031,'slot':0,'raw_count_or_subtype':gold}] if gold else []}
class World:
    def __init__(self):self.containers=[container(0,2853,18)];self.before=True;self.many=False
    def scan(self):return copy.deepcopy(self.containers)
    def player(self):return {'x':100,'y':100,'z':7}
    def corpses_at(self,p):return [copy.deepcopy(BODY)]*(2 if self.many else 1)
    def nearby_corpses(self,*args):return [] if self.before else self.corpses_at(POSITION)
    def nearby_rats(self):return [{'id':42,'object':'0x987654','position':POSITION,'hp':100}]
    def rats_on_floor(self):return self.nearby_rats()
    def rat(self,rid):return None
def result(**extras):
    return {'exit_code':0,'report':dict.fromkeys(('started','returned','context_restored','stack_slot_restored','detached'),True)|{'diagnostics':[]}|extras}
def exercise(fail_move=False,full_cycle=False):
    w=World();w.before=full_cycle;calls=[];report={}
    def runner(command,prefix):
        name=Path(command[0]).name;calls.append(name)
        if name=='OTC_Rat_Attack_Once.exe':
            w.before=False
            return result(attack_requested=True,attack_returned=True,owned_reference_handled=True,rat_id=42,rat_object='0x987654',rat_position_at_request=POSITION)
        if name=='OTC_Corpse_Open_Once.exe':
            w.containers.append(container(1,3994,3));return result(raw_rax_at_return='0x1')
        if fail_move:return {'exit_code':2,'report':{'confirmed':False}}
        w.containers[0]=container(0,2853,21);w.containers[1]=container(1,3994,0)
        return {'exit_code':0,'report':{'confirmed':True}}
    try:
        if full_cycle:cycle(Memory(),w,123,456,0,1,report,lambda:False,runner)
        else:open_and_collect(Memory(),w,123,456,0,report,lambda:False,runner=runner)
    except ValueError:
        if not fail_move:raise
        assert len(calls)==2 and not report.get('confirmed');return
    assert not fail_move and report['confirmed'] and report['collected']==3
    assert len(calls)==(3 if full_cycle else 2)
exercise();exercise(fail_move=True);exercise(full_cycle=True)
w=World();assert fresh_corpse(w,POSITION,{BODY['object']}) is None
w.many=True
try:fresh_corpse(w,POSITION,set())
except ValueError:pass
else:raise AssertionError('Ambiguous bodies accepted')
print('PASS: ciclo completo simulado, abertura nova, totais e parada sem repetir transferencia falha')

# Duas pilhas: cada chamada usa o conteudo atual, sem reutilizar o slot anterior.
w=World();w.before=False;calls=[];report={}
def multiple(command,prefix):
    calls.append(command)
    if len(calls)==1:
        source=container(1,3994,2)
        source['items'].append({'id':3031,'slot':1,'raw_count_or_subtype':5})
        source['items'].append({'id':3552,'slot':2,'raw_count_or_subtype':1})
        source['item_count']=3;w.containers.append(source)
        return result(raw_rax_at_return='0x1')
    count=int(command[-1]);assert count==(2 if len(calls)==2 else 5)
    w.containers[0]['items'][0]['raw_count_or_subtype']+=count
    w.containers[1]['items'].pop(0)
    return {'exit_code':0,'report':{'confirmed':True}}
open_and_collect(Memory(),w,123,456,0,report,lambda:False,runner=multiple)
assert report['confirmed'] and report['collected']==7 and len(calls)==3
assert w.containers[1]['items'][0]['id']==3552

# Um retorno com ID de janela ja aberta nao autoriza transferencia.
w=World();w.before=False;calls=[]
def reused(command,prefix):
    calls.append(command);return result(raw_rax_at_return='0x0')
try:open_and_collect(Memory(),w,123,456,0,{},lambda:False,runner=reused)
except ValueError:pass
else:raise AssertionError('Reused container accepted')
assert len(calls)==1
print('PASS: recolhe duas pilhas, preserva outro item e rejeita janela existente sem mover moedas')

# Perseguicao: rato afasta dois tiles, jogador acompanha e so abre ao alcance.
class Chase(World):
    def __init__(self,scenario='normal'):
        super().__init__();self.phase=False;self.tick=0;self.scenario=scenario
        self.destination={'x':104,'y':100,'z':7}
        self.old_body=copy.deepcopy(BODY);self.old_body.update(object='0x555555',position=self.destination)
        self.new_body=copy.deepcopy(BODY);self.new_body['position']=self.destination
    def player(self):
        if not self.phase:return super().player()
        threshold=3 if self.scenario=='late_approach' else 1
        return {'x':101 if self.tick<threshold else 103,'y':100,'z':8 if self.scenario=='floor' else 7}
    def nearby_rats(self):
        if self.scenario=='far_start':return [{'id':42,'object':'0x987654','position':self.destination,'hp':100}]
        return super().nearby_rats()
    def rat(self,rid):
        self.tick+=1
        if self.tick<=2:
            return {'id':42,'object':'0x000000' if self.scenario=='reuse' else '0x987654',
                    'position':self.destination,'hp':30}
        if self.scenario=='cached_dead':
            raise RatDataError({'id':42,'actual_id':42,'object':'0x987654','control':'0x987650','hp':0,
                                'position':{'x':65535,'y':65535,'z':255},'reason':'invalid_position_or_health'})
        return None
    def corpses_at(self,p):
        if p!=self.destination:return []
        return [copy.deepcopy(self.old_body)]+([copy.deepcopy(self.new_body)] if self.tick>=3 else [])

for scenario in ('normal','far_start','late_approach','cached_dead','floor','reuse'):
    w=Chase(scenario);calls=[];report={}
    def chase_runner(command,prefix):
        name=Path(command[0]).name;calls.append(name)
        if name=='OTC_Rat_Attack_Once.exe':
            w.phase=True
            return result(attack_requested=True,attack_returned=True,owned_reference_handled=True,rat_id=42,
                          rat_object='0x987654',rat_position_at_request=w.rats_on_floor()[0]['position'])
        if name=='OTC_Corpse_Open_Once.exe':
            assert w.player()['x']==103 and command[-1]==BODY['object']
            w.containers.append(container(1,3994,3));return result(raw_rax_at_return='0x1')
        w.containers[0]=container(0,2853,21);w.containers[1]=container(1,3994,0)
        return {'exit_code':0,'report':{'confirmed':True}}
    try:cycle(Memory(),w,123,456,0,2,report,lambda:False,chase_runner)
    except ValueError:
        if scenario in ('normal','far_start','late_approach','cached_dead'):raise
        assert len(calls)==1 and not report.get('confirmed')
    else:
        assert scenario in ('normal','far_start','late_approach','cached_dead') and report['confirmed'] and len(calls)==3
        assert '0x555555' in report['old_corpse_objects']
        assert len(report['tracking'])==3
        assert report['target_world_exit_reads']>=2
        if scenario=='cached_dead':assert report['target_world_exit']['kind']=='cached_dead_off_map'
print('PASS: perseguicao com distancia temporaria, corpo antigo em tile novo, andar e ID reutilizado')

# Reproduz exatamente a identidade e o estado terminal do relatorio enviado pelo usuario.
selected={'id':1073773072,'object':'0x23fbae950a0','control':'0x23fbae95090'}
terminal=dict(selected,actual_id=1073773072,hp=0,position={'x':65535,'y':65535,'z':255},reason='invalid_position_or_health')
class TerminalWorld:
    def __init__(self,details):self.details=details
    def rat(self,rid):raise RatDataError(self.details)
assert tracked_observation(TerminalWorld(terminal),selected)[1]['kind']=='cached_dead_off_map'
for changed in ({'hp':100},{'object':'0x111111'},{'control':'0x222222'},{'actual_id':43},
                {'position':{'x':1,'y':1,'z':255}},{'reason':'pointer_pair_changed_or_null_control'}):
    try:tracked_observation(TerminalWorld(terminal|changed),selected)
    except RatDataError:pass
    else:raise AssertionError(('Wrong terminal accepted',changed))
assert fresh_corpse(World(),POSITION,{BODY['object']}) is None
print('PASS: estado real de Rat morto em cache; rejeita vida positiva, identidade/controle trocados e posicao diferente')
