from pathlib import Path
import ctypes,tempfile
lib=ctypes.CDLL(r'C:\Users\Usuario\Documents\Codex\Crome_Bateria_5\lua51.dll')
lib.luaL_newstate.restype=ctypes.c_void_p;lib.luaL_openlibs.argtypes=[ctypes.c_void_p];lib.luaL_loadbuffer.argtypes=[ctypes.c_void_p,ctypes.c_char_p,ctypes.c_size_t,ctypes.c_char_p];lib.lua_pcall.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_int,ctypes.c_int];lib.lua_tolstring.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_void_p];lib.lua_tolstring.restype=ctypes.c_char_p;lib.lua_close.argtypes=[ctypes.c_void_p]
root=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(dir=root)as temp:
 L=lib.luaL_newstate();lib.luaL_openlibs(L)
 def run(s):
  b=s.encode();e=lib.luaL_loadbuffer(L,b,len(b),b'nav_learning_test')
  if not e:e=lib.lua_pcall(L,0,0,0)
  if e:raise AssertionError(lib.lua_tolstring(L,-1,None).decode())
 try:
  run('clock=0;os.mtime=function()return clock end; Nav=dofile([['+(root/'navigation.lua').as_posix()+']]);Policy=dofile([['+(root/'policy.lua').as_posix()+']]);temp=[['+Path(temp).as_posix()+']]')
  run('''
  local prior={};for x=1,7 do for y=1,5 do if x~=4 or y==1 then prior[x..','..y..',7']=true end end end
  local n=Nav.new{domain='map-A',prior=prior,path=temp..'/nav.tsv'}
  local start={x=2,y=4,z=7};local target={x=6,y=4,z=7}
  local plan=n:plan(start,target);assert(plan.status=='ROUTE'and plan.length==10 and plan.first~=nil,'route must go around wall')
  assert(n:plan(start,{x=6,y=4,z=8}).status=='DIFFERENT_FLOOR')
  local block={x=2,y=3,z=7};n:observe(block,false,'CREATURE');assert(not n:walkable(block)and n.tiles[Nav.key(block)]==nil)
  clock=2001;assert(n:walkable(block),'temporary occupant must expire without becoming wall')
  n:visit(start);n:sent(start,block);assert(n.confirmed==0 and not n.visited[Nav.key(block)])
  assert(n:observePosition(start).status=='SENT');local ok=pcall(function()n:sent(start,block)end);assert(not ok,'second step while pending must be rejected')
  local done=n:observePosition(block);assert(done.status=='COMPLETED'and done.novel and n.confirmed==1)
  n:sent(block,start);done=n:observePosition(start);assert(not done.novel)
  n:sent(start,block);clock=5002;assert(n:observePosition(start).status=='UNCERTAIN'and n.confirmed==2)
  n:sent(start,block);assert(n:observePosition({x=99,y=99,z=7}).reason=='UNEXPECTED_DISPLACEMENT')
  assert(n:save());local loaded=Nav.new{domain='map-A',prior=prior,path=temp..'/nav.tsv'};assert(loaded.loaded>0 and loaded.visited[Nav.key(block)]==1 and not loaded:visit(block))
  assert(not pcall(function()Nav.new{domain='map-B',path=temp..'/nav.tsv'}end))
  local p=Policy.new{domain='map-A',path=temp..'/q.tsv',atomic=Nav.atomic};p.epsilon=0
  local bad={id='EXPLORE:bad',score=0};local good={id='EXPLORE:good',score=0}
  assert(p:choose('s',{bad,good}).id==bad.id);assert(p.pending.confirmed==0 and p.pending.reward==0)
  assert(not pcall(function()p:choose('s',{good})end));p:reward('HP_LOSS',-2);p:finish('REJECTED','next',{},true)
  assert(p:choose('s',{bad,good}).id==good.id);p:confirmed(true);p:finish('COMPLETED','next',{},true)
  assert(p:value('s',good.id)>p:value('s',bad.id));p:choose('s',{good});p:confirmed(false);assert(p.pending.reward==0,'repeat tile must not earn exploration reward')
  local repeated=p:finish('COMPLETED','next',{},true);assert(repeated.reward<=0,'completed revisits must not farm reward');assert(p:save());local p2=Policy.new{domain='map-A',path=temp..'/q.tsv',atomic=Nav.atomic};assert(p2.updates==3 and math.abs(p2:value('s',good.id)-p:value('s',good.id))<1e-9)
  assert(not pcall(function()Policy.new{domain='map-B',path=temp..'/q.tsv',atomic=Nav.atomic}end))
  p2.q.next={safe=-1,unsafe=100};p2.epsilon=0;p2:choose('bootstrap',{{id='test'}});local result=p2:finish('COMPLETED','next',{{id='safe'}},false);assert(result.value<0,'masked unsafe Q must not bootstrap')
  local probe=Policy.new{domain='test',atomic=Nav.atomic};probe.epsilon=1;probe.q.s={a=2,b=0};local calls=0;probe.random=function()calls=calls+1;return calls==1 and 0 or .99 end;local chosen,random,q=probe:choose('s',{{id='a'},{id='b'}});assert(chosen.id=='b'and random and q==0,'choice telemetry must report selected Q')
  assert(not pcall(function()probe:reward('invalid',0/0)end))
  print('PASS: detour, floor separation, temporary blockage TTL, pending gate, confirmed-only novelty, timeout, displacement, persistent visits/Q, domain isolation, goal preference update, no repeated-tile reward, masked bootstrap')
  ''')
 finally:lib.lua_close(L)
