CromeCaveAssignments=CromeCaveAssignments or{}
CromeSectorReservations=CromeSectorReservations or{}
CromeSectorCooldown=CromeSectorCooldown or{}
local Autonomy=dofile('data/crome_race/baseline/autonomy.lua')
local Policy=dofile('data/crome_race/tibinha_policy.lua')
local Router=dofile('data/crome_race/baseline/router.lua')
local Market=dofile('data/crome_race/baseline/market.lua')
local Scavenge=dofile('data/crome_race/scavenge.lua')
CromeDangerSpots=CromeDangerSpots or{}
local Progression=dofile('data/crome_race/progression.lua')
local ArmRoutes=dofile('data/crome_race/arm_routes.lua')
local Brain={};Brain.__index=Brain
local function now()return os.mtime()end
local function cp(p)return Router.copy(p)end
local function key(p)return Router.key(p)end
local function distance(a,b)return Router.distance(a,b)end
local function fed(p)local c=p:getCondition(CONDITION_REGENERATION,CONDITIONID_DEFAULT);return c and c:getTicks()>10000 end
local tier={rat=1,['cave rat']=1,bug=1,spider=1,wolf=2,snake=2,troll=2,skeleton=3,orc=3,bear=3}
function Brain:unsafeGoal(p,q,knownRank)
 if not q then return false end
 local rank=knownRank or self:huntTier(p)
 for dx=-1,1 do for dy=-1,1 do
  local bucket=CromeThreatIndex[q.z..':'..(math.floor(q.x/8)+dx)..','..(math.floor(q.y/8)+dy)]or{}
  for _,s in ipairs(bucket)do if(tier[s.name]or 10)>rank and math.max(math.abs(q.x-s.pos.x),math.abs(q.y-s.pos.y))<=6 and Position(q.x,q.y,q.z):isSightClear(Position(s.pos.x,s.pos.y,s.pos.z),true)then return true end end
 end end
 for k,s in pairs(CromeDangerSpots)do
  if s.untilAt>os.time()and s.pos.z==q.z and(tier[s.name]or 10)>rank and math.max(math.abs(q.x-s.pos.x),math.abs(q.y-s.pos.y))<=6 then
   local here=p:getPosition();local visible=here.z==s.pos.z and math.abs(here.x-s.pos.x)<=7 and math.abs(here.y-s.pos.y)<=5 and (distance(here,q)<=1 or Position(here.x,here.y,here.z):isSightClear(Position(s.pos.x,s.pos.y,s.pos.z),true))
   local present=not visible
   if visible then
    self.dangerScan=self.dangerScan or{at=-10000,list={}}
    if now()-self.dangerScan.at>=500 then self.dangerScan={at=now(),list=self:enemies(p)}end
    for _,c in ipairs(self.dangerScan.list)do if c:getName():lower()==s.name and distance(c:getPosition(),s.pos)<=6 then present=true;break end end
    if not present and Position(here.x,here.y,here.z):isSightClear(Position(s.pos.x,s.pos.y,s.pos.z),true)then CromeDangerSpots[k]=nil;self.emit('DANGER_CLEARED_BY_OBSERVATION',{name=s.name,position=s.pos})end
   end
   if present and Position(q.x,q.y,q.z):isSightClear(Position(s.pos.x,s.pos.y,s.pos.z),true)then return true end
  end
 end
 return false
end
function Brain:markDanger(q,name)
 name=name:lower();local k=name..'@'..q.z..':'..math.floor(q.x/8)..','..math.floor(q.y/8)
 CromeDangerSpots[k]={pos=cp(q),name=name,untilAt=os.time()+86400}
end
function Brain:returnExit(p)
 local here=p:getPosition();local e=self.router.entry[here.z]
 if not e or not e.parent or e.parent.z==here.z then return end
 local best=nil;local gap=nil
 for _,ref in ipairs(self.wiki.accesses)do
  if ref.pos.z==here.z and ref.delta==e.parent.z-here.z then
   local link=self.router.links[key(ref.pos)]
   if link and link.z==e.parent.z and distance(link,e.parent)<=2 then
    local d=distance(ref.pos,here);if not gap or d<gap then gap=d;best=ref end
   elseif not best and distance(ref.pos,e.arrival)<=2 then best=ref;gap=distance(ref.pos,here)+100 end
  end
 end
 if best then
  local id='RETREAT_ACCESS:'..key(best.pos)
  if((self.failed or{})[id]or 0)>now()or(self.memory and not self.memory:allowed(id))then return end
  return{id=id,kind='RETREAT_ACCESS',access=best,goal=cp(best.pos),score=150,routeStatus='KNOWN_REVERSE_ACCESS'}
 end
end

function Brain.new(a,cfg,wiki,prior,inventory,emit)
 if not CromeThreatIndex then
  CromeThreatIndex={}
  for _,spawn in ipairs(wiki.spawns)do local kind=MonsterType(spawn.name);if kind and kind:isHostile()then local q=spawn.pos;local k=q.z..':'..math.floor(q.x/8)..','..math.floor(q.y/8);CromeThreatIndex[k]=CromeThreatIndex[k]or{};table.insert(CromeThreatIndex[k],spawn)end end
  local f=io.open(cfg.memoryRoot..'/danger_spots.tsv','r')
  if f then for line in f:lines()do local x,y,z,name,untilAt=line:match('^(%d+)\t(%d+)\t(%d+)\t([^\t]+)\t(%d+)$');if x and tonumber(untilAt)>os.time()then CromeDangerSpots[name..'@'..z..':'..math.floor(tonumber(x)/8)..','..math.floor(tonumber(y)/8)]={pos={x=tonumber(x),y=tonumber(y),z=tonumber(z)},name=name,untilAt=tonumber(untilAt)}end end;f:close()end
 end

 local self=setmetatable({a=a,cfg=cfg,wiki=wiki,inventory=inventory,emit=emit,failed={},failureCounts={},lastXp=0,lastHp=0,loots={},recent={},lastCheck=now(),lastXpAt=now(),floorEnteredAt=now()},Brain)
 a.spawn=cp(Player(a.id):getPosition());self.router=Router.new(a,wiki,prior,cfg,inventory,emit)
 self.router.avoid=function(q)local g=self.goal;if g and(g.kind=='ESCAPE'or g.kind=='RETREAT_ACCESS'or g.kind=='RECOVER'or g.kind=='RETURN')then return false end;return self:unsafeGoal(Player(a.id),q)end
 local stem=cfg.memoryRoot..'/'..a.name:gsub('[^%w_]','_')
 self.policy=Policy.new{domain=cfg.domain,path=stem..'_policy.tsv',atomic=self.router.nav.atomic,seed=a.index*7919+17,priorWeight=.25,actorName=a.name,emit=emit,tibinhaEnabled=(a.variant=='tibinha')or(cfg.tibinhaActors and cfg.tibinhaActors[a.name]or false),snapshotFn=function()local pl=a.id and Player(a.id);if not pl then return nil end;local pos=pl:getPosition();return{level=pl:getLevel(),xp=pl:getExperience(),hp=pl:getHealth(),maxHp=pl:getMaxHealth(),money=pl:getMoney(),position={x=pos.x,y=pos.y,z=pos.z},goal=self.goal and self.goal.id or'NONE'}end}
 local p=Player(a.id);local entry=self.router.entry[8]
 if p:getLevel()<3 and p:getPosition().z>7 and entry and entry.access.z==7 then CromeCaveAssignments[a.name]={entry=key(entry.access),entered=true,untilAt=now()+120000}end
 self.lastXp=p:getExperience();self.lastHp=p:getHealth();self.stats={played=0,deaths=0,earned=0,spent=0,moves=0,transitions=0,loops=0,metas=0,uncertain=0}
 self.stem=stem
 self.memory=Autonomy.new{domain=cfg.domain,path=stem..'_autonomy.tsv',atomic=self.router.nav.atomic}
 self.memory:arrive(p:getPosition());self.lastRegion=Autonomy.region(p:getPosition());self.lastUseful=now();self.lastMoney=p:getMoney()
 self.selectionStats={filtered=0};self.lastFilterLog={}
 self.progression=Progression.new(self);self.circuit=ArmRoutes.new(self)

 local f=io.open(stem..'_stats.tsv','r')
 if f then
  assert(f:read('*l')=='CROME_RACE_STATS_1\t'..cfg.domain,'Stats domain mismatch')
  for l in f:lines()do local k,v=l:match('^(%w+)\t(%d+)$');if k then self.stats[k]=tonumber(v)end end;f:close()
 end
 self.emit('MEMORY_LOADED',{tiles=self.router.nav.loaded,updates=self.policy.updates,playedMillis=self.stats.played,autonomyRows=self.memory.loaded})
 return self
end
function Brain:state(p)
 local armor=0;local weapon=0;for _,slot in ipairs({1,4,5,6,7,8})do local i=p:getSlotItem(slot);if i then armor=armor+i:getType():getArmor();weapon=math.max(weapon,i:getType():getAttack())end end
 local q=p:getPosition()
 return table.concat({'r1',q.z,math.floor(q.x/16),math.floor(q.y/16),p:getLevel(),math.min(4,math.floor(p:getHealth()/p:getMaxHealth()*5)),math.floor(armor/3),math.floor(weapon/4),fed(p)and 1 or 0},':')
end
function Brain:enemies(p)
 local list={};local here=p:getPosition()
 for _,c in ipairs(Game.getSpectators(here,false,false,7,7,5,5))do if c:isMonster()and c:getType():isHostile()and c:getPosition().z==here.z then list[#list+1]=c end end
 return list
end
function Brain:peak(monster)
 local peak=0
 for _,a in ipairs(monster:getType():getAttackList())do peak=math.max(peak,math.abs(a.minCombatValue or 0),math.abs(a.maxCombatValue or 0))end
 return math.max(peak,5)
end
function Brain:preferredSpecies(p,name)
 if p:getLevel()<3 then return true end
 if self.progression and not self.progression:ready(p)then return name=='rat'or name=='cave rat'or name=='bug'or name=='spider'end
 if name=='rat'or name=='cave rat'then return self.progression and self.progression:ratFallback(p)or false end
 return name=='bug'or name=='spider'or name=='wolf'or(p:getLevel()>=5 and(name=='snake'or name=='troll'))
end
function Brain:reserveSector(goal)
 local p=Player(self.a.id)
 if goal and goal.kind=='ENTER'and goal.access.pos.z==7 and p and p:getLevel()<3 then
  local k=key(goal.access.pos);local current=CromeCaveAssignments[self.a.name]
  if not current or current.entry~=k then CromeCaveAssignments[self.a.name]={entry=k,entered=false,untilAt=now()+120000}else current.untilAt=now()+120000 end
 end
 if goal and goal.kind=='SECTOR'then CromeSectorReservations[goal.id]={owner=self.a.name,untilAt=now()+30000}end
end
function Brain:huntTier(p)
 local attack=0;local armor=0
 for _,slot in ipairs({1,4,5,6,7,8})do local item=p:getSlotItem(slot);if item then local t=item:getType();attack=math.max(attack,t:getAttack());armor=armor+t:getArmor()end end
 local rank=1
 if p:getLevel()>=3 or(p:getLevel()>=2 and attack>=12 and armor>=6)then rank=2 end
 if p:getLevel()>=5 and attack>=18 and armor>=10 then rank=3 end
 if self.progression and not self.progression:ready(p)then rank=math.min(rank,1)end
 return rank
end
function Brain:safeTarget(p,c)
 local name=c:getName():lower();local info=self.wiki.monsters[name];local level=tier[name]or 10
 if not self:preferredSpecies(p,name)then return false end
 if not info or level>self:huntTier(p)then return false end
 if info.poison and(not fed(p)or p:getHealth()/p:getMaxHealth()<.85)then return false end
 local focus=c:getTarget();if focus and focus:isPlayer()and focus:getId()~=p:getId()and distance(p:getPosition(),c:getPosition())>1 then return false end
 return p:getHealth()>self:peak(c)*3+35
end
function Brain:candidates(p)
 local list={};local offered={};local here=p:getPosition();local health=p:getHealth()/p:getMaxHealth();local inv=self.inventory;local enemies=self:enemies(p);local rank=self:huntTier(p)
 local function add(g,score)
  if offered[g.id]then return end
  if g.kind=='BUY'and self.progression and not self.progression:purchaseWorthTrip(p,g)then return end
  if self.circuit and(g.kind=='SECTOR'or g.kind=='EXPLORE'or g.kind=='ENTER')and not g.circuit then return end
  if self.progression then
   local allowed=self.progression:allow(p,g);if not allowed then return end
   if g.kind=='SECTOR'then score=self.progression:areaScore(p,g,score)end
  end
  if (self.failed[g.id]and self.failed[g.id]>now())or not self.memory:allowed(g.id)then return end
  if g.kind=='EXPLORE'and(g.goal.z~=here.z or math.abs(g.goal.x-here.x)+math.abs(g.goal.y-here.y)>30)then return end
  if(g.kind=='EXPLORE'or g.kind=='SECTOR'or g.kind=='ENTER'or g.kind=='CIRCUIT')and self:unsafeGoal(p,g.goal,rank)then return end
  if g.kind=='SECTOR'and(CromeSectorCooldown[g.id]or 0)>now()then return end
  local reservation=g.kind=='SECTOR'and CromeSectorReservations[g.id]
  if reservation and reservation.owner~=self.a.name and reservation.untilAt>now()then return end
  local regional=g.goal and self.memory:value(g.goal)or 0
  offered[g.id]=true;g.score=score+((g.kind=='HUNT'or g.kind=='SECTOR'or g.kind=='EXPLORE'or g.kind=='ENTER')and regional or 0);list[#list+1]=g
 end
 if p:getHealth()<65 then
  add({id='RECOVER:Cipfried',kind='RECOVER',goal=self.cfg.temple},100);if #list==0 then list[1]={id='WAIT',kind='WAIT',goal=cp(here),score=-10}end;return list
 end
 
 local food=inv:foodCount(p)
 if not fed(p)and food==0 then
  local options={}
  for _,resource in ipairs(self.wiki.resources or{})do
   if resource.pos.z==here.z then
    local id='FORAGE:'..key(resource.pos)
    if distance(here,resource.pos)<=5 and self.router:find(p,resource.pos,function(i)return i:getId()==2785 or i:getId()==2677 end)then self.memory.failures[id]=nil end
    if self.memory:allowed(id)then
    local ready=true
    if distance(here,resource.pos)<=5 then
     ready=self.router:find(p,resource.pos,function(i)return i:getId()==2785 or i:getId()==2677 end)~=nil
     if not ready then self.memory:outcome('FORAGE:'..key(resource.pos),'UNCERTAIN','BUSH_NOT_READY')end
    end
    if ready then local score=distance(here,resource.pos);options[#options+1]={resource=resource,score=score}end
   end end
  end
  table.sort(options,function(a,b)return a.score<b.score end)
  for i=1,math.min(4,#options)do local q=options[i].resource.pos;add({id='FORAGE:'..key(q),kind='FORAGE',goal=q},health<.8 and 20-options[i].score*.05 or 4-options[i].score*.02)end
 end
 local safeCombatAvailable=false;for _,c in ipairs(enemies)do if self:safeTarget(p,c)then safeCombatAvailable=true;break end end
 if health<.6 and fed(p)and not safeCombatAvailable then
  local rest=(#enemies==0 and not p:getTile():hasFlag(TILESTATE_PROTECTIONZONE))and cp(here)or{x=32097,y=32212,z=7}
  add({id='REST:'..Autonomy.region(rest),kind='REST',goal=rest},30)
 end
 local upgrade=inv:upgrade(p)
 if upgrade then add({id='EQUIP:'..upgrade.row.id,kind='EQUIP',upgrade=upgrade},40)end
 for _,sale in ipairs(inv:sellOptions(p))do
  local shop=sale.vendor.pos;local nearby=shop.z==here.z and math.max(math.abs(shop.x-here.x),math.abs(shop.y-here.y))<=3
  if nearby or sale.count>=5 or(sale.price or 0)*sale.count>=20 or not inv:destination(p,sale.item)or p:getFreeCapacity()<5000 then
   sale.id='SELL:'..sale.name..'@'..sale.vendor.name;sale.goal=self.router:vendorGoal(p,sale.vendor.pos);add(sale,6+math.min(4,sale.count*.2)-(math.abs(here.x-sale.goal.x)+math.abs(here.y-sale.goal.y)+math.abs(here.z-sale.goal.z)*25)*.04)
  end
 end
 if self.progression and #enemies==0 then for _,q in ipairs(self.progression:questGoals(p))do add(q,q.score)end elseif self.progression then self.progression.questOptions={}end
 local purchases=inv:buyOptions(p)
 if self.progression then for _,g in ipairs(self.progression:toolGoals(p))do purchases[#purchases+1]=g end end
 for _,buy in ipairs(purchases)do
  buy.id='BUY:'..buy.name..'@'..buy.vendor.name;buy.goal=self.router:vendorGoal(p,buy.vendor.pos)
  local score=4+math.min(4,buy.gain/(buy.price+1)*4)
  if self.progression then score=self.progression:purchaseScore(p,buy,score)end
  add(buy,score)
 end
 local reachableCombatAvailable=false
 for _,c in ipairs(enemies)do
  if self:safeTarget(p,c)then local pos=c:getPosition();local plan=self.router.nav:plan(here,pos,{preview=true,adjacent=true,limit=700,radius=12})
   if plan.status=='ROUTE'or plan.status=='REACHED'then
    reachableCombatAvailable=true;local info=self.wiki.monsters[c:getName():lower()];add({id='HUNT:'..c:getName():lower()..'@'..math.floor(pos.x/8)..','..math.floor(pos.y/8)..','..pos.z,kind='HUNT',target=c:getId(),goal=cp(pos)},12+info.experience*.03-distance(here,pos)*.05)
   end
  end
 end
 if self.circuit and not reachableCombatAvailable and health>=.6 and #self.loots==0 then local g=self.circuit:goal(p);if g then add(g,g.score)end end
 if health>=.6 then
  if here.z==7 then
   local peers=Game.getSpectators(here,false,true,7,7,5,5)
   for _,entry in ipairs(self.wiki.entrances)do
    local ref=nil;for _,v in ipairs(self.wiki.accesses)do if key(v.pos)==key(entry.pos)and v.id==entry.id then ref=v;break end end
    if ref and(not entry.species or p:getLevel()>=3)then
     local congestion=0;local vx,vy=ref.pos.x-here.x,ref.pos.y-here.y
     local assigned=0;for name,claim in pairs(CromeCaveAssignments)do if name~=self.a.name and claim.entry==key(ref.pos)and claim.untilAt>now()then assigned=assigned+1 end end
     for _,peer in ipairs(peers)do if peer:getId()~=p:getId()then local q=peer:getPosition();if(q.x-here.x)*vx+(q.y-here.y)*vy>0 then congestion=congestion+1 end end end
     local preferred=0
     if p:getLevel()>=3 then
      local destination=self.router.links[key(ref.pos)]or{x=ref.pos.x,y=ref.pos.y,z=ref.pos.z+ref.delta}
      for _,spawn in ipairs(self.wiki.spawns)do if spawn.pos.z==destination.z and distance(spawn.pos,destination)<=30 and self:preferredSpecies(p,spawn.name)and(tier[spawn.name]or 10)<=rank then preferred=preferred+1 end end
     end
     if entry.species=='wolf'and p:getLevel()>=3 then preferred=math.max(preferred,8)end
     if p:getLevel()<3 or preferred>0 then add({id='ENTER:'..key(ref.pos),kind='ENTER',access=ref,goal=ref.pos},(p:getLevel()<3 and 10 or 4)+math.min(8,preferred)-assigned*.8-distance(here,ref.pos)*.025-congestion*.4+self.policy:random()*1.1)end
    end
   end
  else
   local spots={}
   for _,spawn in ipairs(self.wiki.spawns)do if spawn.pos.z==here.z and self:preferredSpecies(p,spawn.name)and(tier[spawn.name]or 10)<=rank and self.memory:allowed('SECTOR:'..spawn.name..'@'..math.floor(spawn.pos.x/8)..','..math.floor(spawn.pos.y/8)..','..spawn.pos.z)then spots[#spots+1]={spawn=spawn,d=distance(here,spawn.pos)}end end
   table.sort(spots,function(a,b)return a.d<b.d end)
   for i=1,math.min(5,#spots)do local s=spots[i].spawn;local region=math.floor(s.pos.x/8)..','..math.floor(s.pos.y/8)..','..s.pos.z
    if distance(here,s.pos)>2 then add({id='SECTOR:'..s.name..'@'..region,kind='SECTOR',goal=s.pos,revisitSeconds=s.seconds or 60},3-spots[i].d*.02+(self.wiki.monsters[s.name].experience or 0)*.02+(tier[s.name]or 1)*.8)end
   end
   if #enemies==0 and now()-math.max(self.lastXpAt,self.floorEnteredAt)>90000 and now()-self.lastUseful>20000 then add({id='RETURN:city',kind='RETURN',goal=self.cfg.temple},3)end
   -- A deeper sewer is considered only after its physical item is visible.
   for _,ref in ipairs(self.wiki.accesses)do if ref.pos.z==here.z and ref.delta==1 and ref.id==430 and distance(here,ref.pos)<=6 then add({id='ENTER:'..key(ref.pos),kind='ENTER',access=ref,goal=ref.pos},4)end end
  end
 end
 if p:getLevel()>=3 and health>=.6 then
  local known={};local bounds=self.cfg.bounds
  for _,spawn in ipairs(self.wiki.spawns)do local q=spawn.pos
   if q.z==here.z and self:preferredSpecies(p,spawn.name)and(tier[spawn.name]or 10)<=rank and q.x>=bounds.minX and q.x<=bounds.maxX and q.y>=bounds.minY and q.y<=bounds.maxY then
    known[#known+1]={spawn=spawn,d=distance(here,q)}
   end
  end
  table.sort(known,function(a,b)return a.d<b.d end)
  local regions={}
  for _,row in ipairs(known)do local spawn=row.spawn;local id='SECTOR:'..spawn.name..'@'..math.floor(spawn.pos.x/8)..','..math.floor(spawn.pos.y/8)..','..spawn.pos.z
   if not regions[id]then regions[id]=true;add({id=id,kind='SECTOR',goal=cp(spawn.pos),sectorName=spawn.name,revisitSeconds=spawn.seconds or 60},16-row.d*.035)end
   if #list>=30 then break end
  end
  -- Leave rat-only floors rather than farming defensive XP there indefinitely.
  if here.z~=7 then
   local nearby=false;for _,row in ipairs(known)do if row.d<=25 and not self:unsafeGoal(p,row.spawn.pos,rank)then nearby=true;break end end
   if not nearby then add({id='RETURN:progression',kind='RETURN',goal=self.cfg.temple},25)end
  end
 end
 if p:getLevel()<3 and health>=.6 and here.z==8 then
  local parent=self.router.entry[here.z]
  for _,hint in ipairs(self.wiki.caveLinks or{})do
   if parent and key(parent.access)==key(hint.from)then
    for _,ref in ipairs(self.wiki.accesses)do if key(ref.pos)==key(hint.via)and ref.id==hint.id then add({id='ENTER:'..key(ref.pos),kind='ENTER',access=ref,goal=ref.pos},9)end end
   end
  end
 end
 if p:getLevel()>=3 and here.z~=7 and health>=.6 then
  for _,entry in ipairs(self.wiki.entrances)do if entry.species=='wolf'and entry.pos.z==here.z then
   for _,ref in ipairs(self.wiki.accesses)do if key(ref.pos)==key(entry.pos)and ref.id==entry.id then add({id='ENTER:'..key(ref.pos),kind='ENTER',access=ref,goal=ref.pos},14-distance(here,ref.pos)*.025)end end
  end end
 end
 local frontiers=self.router.nav:frontiers(here,self.a.spawn,100)
 for i=1,math.min(3,#frontiers)do local f=frontiers[i];add({id='EXPLORE:'..key(f.goal),kind='EXPLORE',goal=f.goal},(p:getLevel()>=3 and -5 or 1)+f.score*.05)end
 -- A reachable visible switch may unlock a route. Re-use is masked after one attempt.
 for dx=-1,1 do for dy=-1,1 do local q={x=here.x+dx,y=here.y+dy,z=here.z};local v=self.router:find(p,q,function(item)return item:getId()==1945 and item:getActionId()==3100 end)
  if v then add({id='SWITCH:'..key(q),kind='SWITCH',goal=q,object=v},18)end
 end end
 -- Inspect visible quest containers; storage changes, not sending use, confirm rewards.
 if health>=.6 and #enemies==0 then
  for dx=-3,3 do for dy=-3,3 do local q={x=here.x+dx,y=here.y+dy,z=here.z}
   local chest=self.router:find(p,q,function(i)return i:getActionId()==65535 and i:getUniqueId()>0 end)
   if chest then local uid=chest:getUniqueId()
    if p:getStorageValue(uid)<1 then add({id='INSPECT:'..uid..'@'..key(q),kind='INSPECT',goal=q,uid=uid,itemId=chest:getId()},9)end
   end
  end end
 end
 -- Preflight a bounded shortlist; inferred cross-floor references remain explicitly provisional.
 if self.progression then list=self.progression:focus(p,list)end
 local decisionState=self:state(p)
 for _,g in ipairs(list)do g.selectionValue=self.policy:value(decisionState,g.id)+g.score*.25 end
 table.sort(list,function(a,b)if a.selectionValue==b.selectionValue then return a.id<b.id end;return a.selectionValue>b.selectionValue end)
 local reachable={}
 for i=1,math.min(12,#list)do local g=list[i]
  local status=g.goal and self.router:assess(p,g.goal,g.kind=='HUNT'or g.kind=='FORAGE'or g.kind=='ENTER'or g.kind=='BUY'or g.kind=='SELL'or g.kind=='RECOVER'or g.kind=='INSPECT')or'REACHABLE'
  if status=='REACHABLE'or status=='REFERENCE_ONLY'then g.routeStatus=status;reachable[#reachable+1]=g;if #reachable>=4 then break end
  else
   self.selectionStats.filtered=self.selectionStats.filtered+1
   if now()-(self.lastFilterLog[g.id]or -20000)>15000 then self.emit('GOAL_FILTERED',{id=g.id,reason=status});self.lastFilterLog[g.id]=now()end
   if status=='NO_ROUTE'or status=='NO_ACCESS'then self.memory:outcome(g.id,'UNCERTAIN',status)end
  end
 end
 if #reachable==0 then
  local entry=self.router.entry[here.z]
  if here.z~=7 and entry and entry.parent and entry.parent.z~=here.z then
   reachable[1]={id='RETURN:known-entry:'..key(entry.arrival),kind='RETURN',goal=cp(entry.parent),score=20,routeStatus='KNOWN_ENTRY_RETURN'}
  else reachable[1]={id='WAIT',kind='WAIT',goal=cp(here),score=-10}end
 end
 return reachable
end
function Brain:finish(p,status,reason,nextAllowed)
 if not self.policy.pending then return end
 local goal=self.goal;if self.circuit then self.circuit:onResult(p,goal,status,reason)end;if self.progression then self.progression:onResult(p,goal,status,reason)end;if goal and goal.kind=='ENTER'and status~='COMPLETED'then local membership=CromeCaveAssignments[self.a.name];if membership and not membership.entered then CromeCaveAssignments[self.a.name]=nil end end;if goal and goal.kind=='SECTOR'then local lease=CromeSectorReservations[goal.id];if lease and lease.owner==self.a.name then CromeSectorReservations[goal.id]=nil end end;if goal and goal.kind=='HUNT'then p:setTarget(nil);p:setFollowCreature(nil)end;if goal then self.memory:outcome(goal.id,status,reason)end;if self.a.trade then Market.release(self.a,p)end;local reward=self.policy:finish(status,self:state(p),nextAllowed or self:candidates(p),false)
 if status=='COMPLETED'then self.stats.metas=self.stats.metas+1 else self.stats.uncertain=self.stats.uncertain+1 end
 self.emit('META_RESULT',{id=goal and goal.id or'?',status=status,reason=reason,reward=reward.reward,q=reward.value,confirmedSteps=reward.confirmed,updates=reward.updates})
 if status~='COMPLETED'and goal then
  local count=(self.failureCounts[goal.id]or 0)+1;self.failureCounts[goal.id]=count
  self.failed[goal.id]=now()+math.min(300000,20000*2^(math.min(count,5)-1))
 elseif goal then self.failureCounts[goal.id]=nil end
 if goal and goal.kind=='ENTER'and status=='COMPLETED'then self.failed[goal.id]=now()+45000 end
 if goal and goal.kind=='SECTOR'and status=='COMPLETED'and reason~='PREFERRED_TARGET_FOUND'then CromeSectorCooldown[goal.id]=now()+math.max(20,goal.revisitSeconds or 30)*1000;self.memory:pause(goal.id,math.max(20,goal.revisitSeconds or 30),'SECTOR_SCOUTED')end
 if goal and goal.kind=='EXPLORE'and status=='COMPLETED'then self.memory:pause(goal.id,600,'ALREADY_EXPLORED')end
 self.goal=nil;self.blockedAt=nil;self.router.path=nil;self.router.pathGoal=nil;self.router.searching=nil;self.router.searchOutcome=nil
end
function Brain:travel(p,goal,adjacent,returning)
 local status=self.router:go(p,goal,adjacent,returning)
 if status=='SEARCH_LIMIT'or status=='SEARCH_TIMEOUT'then if self.goal then self:finish(p,'UNCERTAIN',status,{})end;return status end
 if status=='KNOWN_DANGER'then if self.goal then self:finish(p,'UNCERTAIN',status,{})end;return status end
 if status=='NO_ROUTE'or status=='BLOCKED'or status=='KEY_REQUIRED'or status=='REFERENCE_CHANGED'or status=='NO_ACCESS'or status=='BUDGET_EXHAUSTED'then
  self.blockedAt=self.blockedAt or now()
  if now()-self.blockedAt>((self.goal and self.goal.kind=='RECOVER')and 1000 or 6000)then self.emit('ROUTE_ABANDONED',{status=status,goal=key(goal)});self:finish(p,'UNCERTAIN',status);self.blockedAt=nil end
 else self.blockedAt=nil end
 return status
end
function Brain:loot(p)
 local job=self.loots[1];if not job then return false end
 if now()-job.at>12000 then table.remove(self.loots,1);self.emit('LOOT_EXPIRED',{});return false end
 local here=p:getPosition();local choice=nil;local score=nil
 for dx=-2,2 do for dy=-2,2 do local q={x=job.pos.x+dx,y=job.pos.y+dy,z=job.pos.z}
  if q.z==here.z and math.abs(q.x-here.x)<=7 and math.abs(q.y-here.y)<=5 then
   local tile=Tile(Position(q.x,q.y,q.z))
   for _,item in ipairs(tile and tile:getItems()or{})do
    if item:isContainer()and item:getCorpseOwner()==p:getId()then
     local value=distance(here,q);if item:getSize()>0 then value=value-100 end
     if not score or value<score then choice={body=item,pos=q};score=value end
    end
   end
  end
 end end
 if not choice then return true end
 if distance(here,choice.pos)>1 then self.router:move(p,choice.pos,true);return true end
 local body=choice.body
 for i=0,body:getSize()-1 do
  local item=body:getItem(i);local id=item:getId();local info=self.wiki.items[id]or{}
  local valuable=id==2148 or id==2152 or id==2160 or self.inventory.buyers[id]or(self.wiki.foods[id]and p:getItemCount(id)<2)or(id==2120 and p:getItemCount(id)==0)or(info.attack or 0)>7 or(info.armor or 0)>1
  local dest=self.inventory:destination(p,id)
  if valuable and(dest or not p:getSlotItem(3))then
   local before=p:getItemCount(id);local ok,why=p:cromeTakeLoot(item,dest);local gain=p:getItemCount(id)-before
   if gain>0 then self.emit('LOOT_CONFIRMED',{id=id,count=gain});return true elseif why=='COOLDOWN'then return true end
  end
 end
 if body:getSize()==0 and(self.inventory.buyers[body:getId()])and(self.inventory:destination(p,body:getId())or not p:getSlotItem(3))and p:getItemCount(body:getId())<5 then
  local id=body:getId();local before=p:getItemCount(id);p:cromeTakeWorldItem(body,self.inventory:destination(p,id));if p:getItemCount(id)>before then self.emit('CORPSE_PICKUP_CONFIRMED',{id=id})end
 end
 table.remove(self.loots,1);return true
end
function Brain:yieldNpcSpace(p,anchor)
 if self.router.pending or self.router.nav.pending or self.a.trade then return false end
 local here=p:getPosition();if not anchor then
  for _,c in ipairs(Game.getSpectators(here,false,false,3,3,3,3))do if c:isNpc()then anchor=cp(c:getPosition());break end end
 end
 if not anchor or anchor.z~=here.z then return false end
 local gap=math.max(math.abs(here.x-anchor.x),math.abs(here.y-anchor.y));if gap>3 then return false end
 local best=nil;local score=gap
 for _,d in ipairs({{0,-1,0},{1,0,1},{0,1,2},{-1,0,3},{-1,1,4},{1,1,5},{-1,-1,6},{1,-1,7}})do
  local q={x=here.x+d[1],y=here.y+d[2],z=here.z};local value=math.max(math.abs(q.x-anchor.x),math.abs(q.y-anchor.y))
  local t=Tile(Position(q.x,q.y,q.z))
  if value>score and t and not t:getFieldItem()and not t:getTopCreature()and p:cromeCanStep(Position(q.x,q.y,q.z))then best={q=q,d=d[3]};score=value end
 end
 if best and p:cromeStep(best.d)then self.router.path=nil;self.router.nav:sent(cp(here),best.q);self.emit('NPC_SPACE_YIELD',{position=best.q});return true,best.q end
 return false
end
function Brain:ratTarget(p,threats)
 if self.progression and self.progression:protectGoal(self.goal)then return end
 if self.progression and self.progression:ready(p)and not self.progression:ratFallback(p)and(not self.goal or self.goal.kind~='HUNT')then return end
 local here=p:getPosition()
 for _,c in ipairs(threats)do local name=c:getName():lower()
  if name~='rat'and name~='cave rat'and here:isSightClear(c:getPosition(),true)then return end
 end
 local current=self.goal and self.goal.kind=='HUNT'and Creature(self.goal.target)or p:getTarget()
 if current and not current:isRemoved()and current:isMonster()and current:getHealth()>0 then
  local name=current:getName():lower()
  if(name=='rat'or name=='cave rat')and current:getPosition().z==here.z and distance(here,current:getPosition())<=15 then return current end
 end
 local best=nil;local score=nil
 for _,c in ipairs(threats)do local name=c:getName():lower()
  if(name=='rat'or name=='cave rat')and c:getHealth()>0 and c:getHealth()<c:getMaxHealth()and distance(here,c:getPosition())<=8 then
   local value=c:getHealth()*10+distance(here,c:getPosition())
   if not score or value<score then local path=self.router.nav:plan(here,c:getPosition(),{preview=true,adjacent=true,limit=700,radius=12})
    if path.status=='ROUTE'or path.status=='REACHED'then best=c;score=value end
   end
  end
 end
 return best
end
function Brain:engagedTarget(p,low)
 if self.progression and self.progression:protectGoal(self.goal)then return end
 if low or(self.goal and(self.goal.kind=='HUNT'or self.goal.kind=='ESCAPE'or self.goal.kind=='RETREAT_ACCESS'))then return end
 local target=p:getTarget();if not target or not target:isMonster()or target:isRemoved()then return end
 local species=target:getName():lower();if self.progression and self.progression:ready(p)and not self.progression:ratFallback(p)and(species=='rat'or species=='cave rat')then return end
 local here=p:getPosition();local q=target:getPosition()
 if target:getHealth()<=0 or target:getHealth()>=target:getMaxHealth()or q.z~=here.z or distance(here,q)>8 then return end
 if(tier[target:getName():lower()]or 10)>self:huntTier(p)or p:getHealth()<=math.max(65,self:peak(target)*3+35)then return end
 local plan=self.router.nav:plan(here,q,{preview=true,adjacent=true,limit=600,radius=12})
 if plan.status~='ROUTE'and plan.status~='REACHED'then return end
 return{id='FINISH:'..target:getName():lower()..':'..target:getId(),kind='HUNT',incidental=true,target=target:getId(),goal=cp(q),score=60,at=now(),xp=p:getExperience(),progress=now(),lastUseful=now()}
end
function Brain:execute(p)
 local g=self.goal;local here=p:getPosition()
 if g.kind=='HUNT'then
  local target=Creature(g.target)
  if not target or target:isRemoved()or target:getHealth()<=0 then
   p:setTarget(nil)
   if p:getExperience()>g.xp then self.loots[#self.loots+1]={pos=g.goal,at=now()};self:finish(p,'COMPLETED','XP_OBSERVED')else self:finish(p,'UNCERTAIN','TARGET_DISAPPEARED')end;return
  end
  if target:getPosition().z~=here.z or distance(here,target:getPosition())>15 then p:setTarget(nil);self:finish(p,'UNCERTAIN','TARGET_OUT_OF_VIEW');return end
  if target:getHealth()<(g.targetHp or 99999)then g.targetHp=target:getHealth();g.progress=now();g.lastUseful=now();self.lastUseful=now()end
  g.goal=cp(target:getPosition())
  local wasFollowing=g.following
  local chasing,reason=p:cromeAttackFollow(target)
  g.following=chasing or nil
  if chasing then
   if not wasFollowing then self.emit('HUNT_ATTACK_FOLLOW_CONFIRMED',{target=g.target,mode='OFFENSIVE',chase=true})end
  else self.emit('HUNT_ATTACK_FOLLOW_REJECTED',{target=g.target,reason=reason});self:travel(p,g.goal,true);return end
  if now()-(g.progress or g.at)>18000 then
   if g.ratLocked then
    local path=self.router.nav:plan(here,g.goal,{preview=true,adjacent=true,limit=1000,radius=18})
    if path.status=='NO_ROUTE'then p:setTarget(nil);self:finish(p,'UNCERTAIN','RAT_PHYSICALLY_UNREACHABLE');return end
    g.progress=now()
   else p:setTarget(nil);self:finish(p,'UNCERTAIN','TARGET_NO_PROGRESS');return end
  end
  -- Native follow owns hunting movement; do not issue competing route steps.
 elseif g.kind=='RETREAT_ACCESS'then
  local status=self.router:enter(p,g.access)
  if status=='REFERENCE_CHANGED'or status=='NO_ROUTE'or status=='BLOCKED'then self:finish(p,'UNCERTAIN',status)end
 elseif g.kind=='ENTER'then
  if here.z~=g.access.pos.z then
   local entry=self.router.entry[here.z];local linked=self.router.links[key(g.access.pos)]
   local entered=entry and entry.access and key(entry.access)==key(g.access.pos)
   local nearArrival=linked and linked.z==here.z and math.max(math.abs(here.x-linked.x),math.abs(here.y-linked.y))<=4
   if here.z==g.access.pos.z+g.access.delta and(entered or nearArrival)then self:finish(p,'COMPLETED','ACCESS_ALREADY_OBSERVED')
   else self:travel(p,g.access.pos,true,true)end
   return
  end
  local status=self.router:enter(p,g.access)
  if status=='REFERENCE_CHANGED'then self:finish(p,'UNCERTAIN',status)
  elseif status=='NO_ROUTE'or status=='BLOCKED'or status=='BUDGET_EXHAUSTED'then
   self.blockedAt=self.blockedAt or now();if now()-self.blockedAt>6000 then self:finish(p,'UNCERTAIN',status)end
  else self.blockedAt=nil end
 elseif g.kind=='ESCAPE'then
  if key(here)==key(g.goal)then self:finish(p,'COMPLETED','LOCAL_ESCAPE_OBSERVED');return end
  local directions={{0,-1,0},{1,0,1},{0,1,2},{-1,0,3},{-1,1,4},{1,1,5},{-1,-1,6},{1,-1,7}}
  for _,d in ipairs(directions)do if here.x+d[1]==g.goal.x and here.y+d[2]==g.goal.y then
   if p:cromeCanStep(Position(g.goal.x,g.goal.y,g.goal.z))and p:cromeStep(d[3])then self.router.nav:sent(cp(here),g.goal)end
   return
  end end
  self:finish(p,'UNCERTAIN','ESCAPE_POSITION_CHANGED')
 elseif g.kind=='INSPECT'then
  if self:travel(p,g.goal,true)~='ARRIVED'then return end
  local chest=self.router:find(p,g.goal,function(i)return i:getUniqueId()==g.uid and i:getId()==g.itemId end)
  if not chest then self:finish(p,'UNCERTAIN','INTERACTION_GONE');return end
  if g.sentAt then
   if p:getStorageValue(g.uid)>(g.beforeStorage or -1)then
    if g.rewardItem and p:getItemCount(g.rewardItem)<=(g.beforeRewardCount or 0)then self:finish(p,'UNCERTAIN','QUEST_REWARD_NOT_OWNED');return end
    self.emit('QUEST_REWARD_CONFIRMED',{uid=g.uid,position=g.goal,storage=p:getStorageValue(g.uid)})
    self.policy:reward('QUEST_DISCOVERY',2);self:finish(p,'COMPLETED','QUEST_STORAGE_OBSERVED');return
   end
   if now()-g.sentAt>3000 then local id=g.id;self:finish(p,'UNCERTAIN','QUEST_NO_OBSERVED_CHANGE');self.memory:pause(id,300,'QUEST_NO_OBSERVED_CHANGE')end
  else
   if g.rewardItem and(not self.inventory:destination(p,g.rewardItem)or p:getFreeCapacity()<((self.wiki.items[g.rewardItem]or{}).weight or 0))then self:finish(p,'REJECTED','QUEST_NEEDS_SPACE');return end
   g.beforeStorage=p:getStorageValue(g.uid);if g.rewardItem then g.beforeRewardCount=p:getItemCount(g.rewardItem)end
   if p:cromeUseObject(chest)then g.sentAt=now();self.emit('QUEST_USE_SENT',{uid=g.uid,position=g.goal})end
  end
 elseif g.kind=='EQUIP'then
  local up=g.upgrade;local expected=up.row.id;local worn=p:getSlotItem(up.slot)
  if worn and worn:getId()==expected then self.policy:reward('EQUIPMENT_GAIN',up.gain*.15);self:finish(p,'COMPLETED','EQUIPMENT_OBSERVED');return end
  local ok,why=p:cromeMoveOwned(up.row.ref,up.slot)
  if not ok and why~='COOLDOWN'then self.emit('EQUIP_ATTEMPT_REJECTED',{item=expected,slot=up.slot,reason=why,position=cp(here)});self:finish(p,'REJECTED','EQUIP_REJECTED')end
 elseif g.kind=='BUY'or g.kind=='SELL'then
  if g.kind=='BUY'and g.item~=1988 and p:getItemCount(g.item)>0 then self:finish(p,'COMPLETED','UPGRADE_ALREADY_OWNED');return end
  if g.kind=='SELL'and self.inventory.canSellType and not self.inventory:canSellType(p,g.item)then self:finish(p,'REJECTED','SELL_CONTENT_PROTECTED');return end
  local npcPosition=g.vendor.pos
  for _,c in ipairs(Game.getSpectators(here,false,false,7,7,5,5))do
   if c:isNpc()and c:getName():lower()==g.vendor.name:lower()then npcPosition=cp(c:getPosition());break end
  end
  local talkRange=math.max(math.abs(here.x-npcPosition.x),math.abs(here.y-npcPosition.y))
  if here.z~=npcPosition.z or talkRange>3 or not here:isSightClear(Position(npcPosition.x,npcPosition.y,npcPosition.z),true)then
   if self:travel(p,self.router:vendorGoal(p,npcPosition),false)~='ARRIVED'then return end
  end
  local status,money=Market.tick(self.a,p,g,self.emit)
  if status=='WAIT_NPC'then self:yieldNpcSpace(p,npcPosition);g.lastUseful=now();return end
  if status=='COMPLETED'then
   if money>0 then self.stats.earned=self.stats.earned+money;self.policy:reward('SALE_INCOME',money*.05)else self.stats.spent=self.stats.spent-money;self.policy:reward('PURCHASE_COST',money*.01);self.policy:reward('ITEM_ACQUIRED',.1)end
   self:finish(p,'COMPLETED','TRADE_OBSERVED')
  elseif status=='UNCERTAIN'then self:finish(p,'UNCERTAIN','TRADE_NOT_CONFIRMED')end
 elseif g.kind=='FORAGE'then
  if self:travel(p,g.goal,true)~='ARRIVED'then return end
  local berries=self.router:find(p,g.goal,function(item)return item:getId()==2677 end)
  if berries and(self.inventory:destination(p,2677)or not p:getSlotItem(3))then
   local before=p:getItemCount(2677);p:cromeTakeWorldItem(berries,self.inventory:destination(p,2677))
   if p:getItemCount(2677)>before then self.emit('HARVEST_PICKUP_CONFIRMED',{count=p:getItemCount(2677)-before});self.policy:reward('FOOD_ACQUIRED',.2);self:finish(p,'COMPLETED','FOOD_IN_INVENTORY')end;return
  end
  local bush=self.router:find(p,g.goal,function(item)return item:getId()==2785 end)
  if not bush then self:finish(p,'UNCERTAIN','BUSH_NOT_READY');return end
  p:cromeUseObject(bush)
 elseif g.kind=='RECOVER'then
  if p:getHealth()>=65 then self:finish(p,'COMPLETED','HEAL_NOT_NEEDED');return end
  local cip=nil;for _,v in ipairs(self.wiki.vendors)do if v.name:lower()=='cipfried'then cip=v;break end end
  if not cip then self:finish(p,'UNCERTAIN','HEALER_UNKNOWN');return end
  if self:travel(p,cip.pos,true,true)~='ARRIVED'then return end
  local status=Market.tick(self.a,p,{kind='HEAL',vendor=cip},self.emit)
  if status=='COMPLETED'then self:finish(p,'COMPLETED','HP_RECOVERY_OBSERVED')elseif status=='UNCERTAIN'then self:finish(p,'UNCERTAIN','NPC_HEAL_UNCONFIRMED')end
 elseif g.kind=='REST'then
  if p:getHealth()/p:getMaxHealth()>=.65 then self:finish(p,'COMPLETED','REST_NOT_NEEDED');return end
  local yielded,restPosition=self:yieldNpcSpace(p);if yielded then g.goal=restPosition;return end
  if self:travel(p,g.goal,false,true)~='ARRIVED'then return end
  if p:getTile():hasFlag(TILESTATE_PROTECTIONZONE)then self:finish(p,'UNCERTAIN','NO_REGEN_IN_PZ');return end
  if not fed(p)or p:getHealth()/p:getMaxHealth()>=.65 then self:finish(p,'COMPLETED','REST_BOUNDARY');return end
 elseif g.kind=='SWITCH'then
  if g.object:getId()==1946 then local id=g.id;self:finish(p,'COMPLETED','SHARED_SWITCH_OPEN');self.failed[id]=now()+90000;return end
  local before=g.object:getId();local ok=p:cromeUseObject(g.object)
  if ok and g.object:getId()~=before then self.emit('SWITCH_STATE_OBSERVED',{pos=key(g.goal),id=g.object:getId()});self.policy:reward('INTERACTION_OBSERVED',.2);self:finish(p,'COMPLETED','SWITCH_CHANGED')end
 elseif g.kind=='RETURN'then
  if here.z==g.goal.z and math.max(math.abs(here.x-g.goal.x),math.abs(here.y-g.goal.y))<=4 then self:finish(p,'COMPLETED','CITY_AREA_OBSERVED');return end
  if self:travel(p,g.goal,true,true)=='ARRIVED'then self:finish(p,'COMPLETED','CITY_AREA_OBSERVED')end
 elseif g.kind=='WAIT'then if self:yieldNpcSpace(p)then return end;if now()-g.at>1500 then self:finish(p,'UNCERTAIN','WAIT_BOUNDARY')end
 else
  if self:travel(p,g.goal,false,g.kind=='RETURN')=='ARRIVED'then self:finish(p,'COMPLETED','POSITION_OBSERVED')end
 end
end
-- A blocked strategic exit must not turn into standing still under attack.
function Brain:escapeGoal(p,threats)
 local here=p:getPosition();local entry=self.router.entry[here.z];local anchor=entry and entry.arrival or(here.z==7 and self.cfg.temple or here)
 self.escapeVisits=self.escapeVisits or{};local best=nil;local bestScore=nil
 for _,d in ipairs({{0,-1},{1,0},{0,1},{-1,0},{-1,1},{1,1},{-1,-1},{1,-1}})do
  local q={x=here.x+d[1],y=here.y+d[2],z=here.z}
  if p:cromeCanStep(Position(q.x,q.y,q.z))then
   local danger=0;local separation=10
   for _,c in ipairs(threats)do local pos=c:getPosition();local gap=math.max(math.abs(q.x-pos.x),math.abs(q.y-pos.y));separation=math.min(separation,gap);if gap<=1 then danger=danger+self:peak(c)end end
   if danger<p:getHealth()then
    local recent=self.escapeVisits[key(q)];local repeatCost=recent and now()-recent<15000 and 4 or 0
    local score=separation*4-danger*.5-distance(q,anchor)*.02-repeatCost-((d[1]~=0 and d[2]~=0)and 1 or 0)
    if not bestScore or score>bestScore then bestScore=score;best=q end
   end
  end
 end
 if best then self.escapeVisits[key(best)]=now();return{id='ESCAPE:'..key(best),kind='ESCAPE',goal=best,score=100,routeStatus='LIVE_STEP_CHECKED'}end
end

function Brain:flushMotion()
 local results=self.motionResults or{};self.motionResults={}
 for _,entry in ipairs(results)do local observed=entry.result
  if observed.status=='COMPLETED'then
   self.stats.moves=self.stats.moves+1
   self.memory:record(observed.position,0,0,0,0,0,observed.novel)
   if self.policy.pending and self.goal==entry.goal then
    self.policy:confirmed(observed.novel)
    if observed.novel then self.policy:reward('NAV_NOVELTY_SCALE',-.195)end
   end
   if observed.novel then self.lastUseful=now();if self.goal==entry.goal then self.goal.lastUseful=now()end end
  elseif observed.status=='UNCERTAIN'then self.motionUncertain=observed.reason end
 end
end
function Brain:motion(p)
 local goal=self.goal
 if not goal then return end
 if goal.ratLocked then
  local here=p:getPosition();local target=Creature(goal.target)
  if not target or target:isRemoved()or target:getHealth()<=0 then self.fastNeedsThink=true;return end
  for _,c in ipairs(self:enemies(p))do local name=c:getName():lower()
   if name~='rat'and name~='cave rat'and here:isSightClear(c:getPosition(),true)then goal.ratLocked=nil;self.fastNeedsThink=true;return end
  end
  local ok=p:cromeAttackFollow(target);if not ok then self.fastNeedsThink=true end
  return
 end

 if goal.kind=='HUNT'and goal.following then
  local here=p:getPosition();local burst=0;local unsafe=p:getHealth()<65
  for _,c in ipairs(self:enemies(p))do
   local q=c:getPosition();local gap=math.max(math.abs(here.x-q.x),math.abs(here.y-q.y))
   if gap<=2 and here:isSightClear(q,true)then burst=burst+self:peak(c)end
   if gap<=4 and(tier[c:getName():lower()]or 10)>self:huntTier(p)and here:isSightClear(q,true)then unsafe=true end
  end
  if burst*2>p:getHealth()-25 then unsafe=true end
  if unsafe then p:setTarget(nil);p:setFollowCreature(nil);goal.following=nil;self.fastNeedsThink=true;return end
  local target=Creature(goal.target);local followed=p:getFollowCreature();local attacked=p:getTarget()
  if not target or target:isRemoved()or not followed or followed:getId()~=goal.target or not attacked or attacked:getId()~=goal.target then self.fastNeedsThink=true end
  return
 end
 if p:getHealth()<65 and goal.kind~='RECOVER'and goal.kind~='ESCAPE'and goal.kind~='WAIT'then if goal.following then p:setFollowCreature(nil);goal.following=nil end;self.fastNeedsThink=true;return end
 if self.router.fastOwner~=goal or not self.router.fastRequest or self.router.pending or self.a.trade or self.fastNeedsThink then return end
 if p:getHealth()<self.lastHp then if goal.following then p:setFollowCreature(nil);goal.following=nil end;self.fastNeedsThink=true;return end
 local here=p:getPosition()
 for _,c in ipairs(self:enemies(p))do
  local known=(self.motionKnown or{})[c:getId()];local q=c:getPosition()
  if not known or(math.max(math.abs(here.x-q.x),math.abs(here.y-q.y))<=2 and known~=key(q))then if goal.following then p:setFollowCreature(nil);goal.following=nil end;self.fastNeedsThink=true;return end
 end
 if goal.kind=='HUNT'then
  if goal.following then return end
  local target=Creature(goal.target)
  if not target or target:isRemoved()or key(target:getPosition())~=key(self.router.fastRequest.goal)then self.fastNeedsThink=true;return end
 end
 local nextStep=self.router.path and self.router.path[1]
 if goal.kind~='ESCAPE'and goal.kind~='RETREAT_ACCESS'and nextStep and self:unsafeGoal(p,nextStep.q)then self.fastNeedsThink=true;return end
 local result,status=self.router:pump(p)
 if result then self.motionResults=self.motionResults or{};self.motionResults[#self.motionResults+1]={result=result,goal=goal}end
 if status=='NEEDS_THINK'then self.fastNeedsThink=true end
end

function Brain:tick(p)
 local membership=CromeCaveAssignments[self.a.name]
 if membership then
  if p:getLevel()>=3 or(membership.entered and p:getPosition().z==7)then CromeCaveAssignments[self.a.name]=nil
  else membership.untilAt=now()+120000;if p:getPosition().z>7 then membership.entered=true end end
 end
 self:reserveSector(self.goal)
 self:flushMotion()
 if self.motionUncertain then local reason=self.motionUncertain;self.motionUncertain=nil;if self.goal then self:finish(p,'UNCERTAIN',reason)end end

 local elapsed=math.min(2000,now()-self.lastCheck);self.lastCheck=now();self.stats.played=self.stats.played+elapsed
 local xp=p:getExperience();local hp=p:getHealth();local gain=xp-self.lastXp;local lost=self.lastHp-hp;self.lastXp=xp;self.lastHp=hp
 if gain~=0 then self.lastXpAt=now();self.emit('XP_CHANGED',{delta=gain,xp=xp,level=p:getLevel()});if self.policy.pending then self.policy:reward('XP',gain*.2)end end
 if lost>0 and self.policy.pending then self.policy:reward('HP_LOSS',-lost*.08)end
 if self.progression then self.progression:observe(p,gain,lost)end
 local current=p:getPosition();local money=p:getMoney();local region=Autonomy.region(current)
 if region~=self.lastRegion then self.memory:arrive(current);self.lastRegion=region end
 local idle=(now()-self.lastUseful>15000)and elapsed or 0
 self.stats.autonomyMillis=(self.stats.autonomyMillis or 0)+elapsed;self.stats.autonomyXp=(self.stats.autonomyXp or 0)+math.max(0,gain);self.stats.autonomyIdle=(self.stats.autonomyIdle or 0)+idle
 self.memory:record(current,elapsed,gain,lost,money-self.lastMoney,idle)
 if gain>0 or money>self.lastMoney then self.lastUseful=now();if self.goal then self.goal.lastUseful=now()end end
 self.lastMoney=money
 local observed=self.router:poll(p)
 if observed and observed.status=='SENT'then return end
 if observed and observed.status=='COMPLETED'then
  self.memory:record(p:getPosition(),0,0,0,0,0,observed.novel)
  if observed.novel then self.lastUseful=now();if self.goal then self.goal.lastUseful=now()end end
  if self.policy.pending and observed.novel~=nil then self.policy:confirmed(observed.novel);if observed.novel then self.policy:reward('NAV_NOVELTY_SCALE',-.195)end end
  if observed.kind=='ACCESS'then self.floorEnteredAt=now();self.stats.transitions=self.stats.transitions+1;if self.policy.pending then self.policy:reward('ACCESS_OBSERVED',.5)end;if self.goal and (self.goal.kind=='ENTER'or self.goal.kind=='RETREAT_ACCESS')then self:finish(p,'COMPLETED','FLOOR_OBSERVED')end
  elseif not observed.kind then self.stats.moves=self.stats.moves+1 end
 elseif observed and observed.status=='UNCERTAIN'and self.goal then self:finish(p,'UNCERTAIN',observed.reason)end
 if self.goal and(self.goal.kind=='EXPLORE'or self.goal.kind=='SECTOR'or self.goal.kind=='ENTER'or self.goal.kind=='CIRCUIT')and self:unsafeGoal(p,self.goal.goal)then
  self:finish(p,'UNCERTAIN','KNOWN_DANGER',{});self.router.path=nil
 end
 self.router:observe(p);self.inventory:feed(p,self.a,self.emit)
 if self.goal and self.goal.ratLocked then local target=Creature(self.goal.target);if not target or target:isRemoved()or target:getHealth()<=0 then self:execute(p)end end
 local here=p:getPosition();local threats=self:enemies(p);self.motionKnown={};for _,c in ipairs(threats)do self.motionKnown[c:getId()]=key(c:getPosition())end;local adj={};local burst=0;local dangerousFocus=self:unsafeGoal(p,here)
 for _,c in ipairs(threats)do
  local focus=c:getTarget();local dangerous=(tier[c:getName():lower()]or 10)>self:huntTier(p)or self:peak(c)*3+35>=p:getHealth()
  if dangerous and here:isSightClear(c:getPosition(),true)then
   local q=c:getPosition()
   for dx=-1,1 do for dy=-1,1 do local avoid={x=q.x+dx,y=q.y+dy,z=q.z}
    if key(avoid)~=key(here)then self.router.nav:observe(avoid,false,'THREAT')end
   end end
   self:markDanger(q,c:getName());if distance(here,q)<=5 or(focus and focus:getId()==p:getId())then dangerousFocus=true end
  end
 end
 for _,c in ipairs(threats)do if distance(here,c:getPosition())<=2 and here:isSightClear(c:getPosition(),true)then adj[#adj+1]=c;burst=burst+self:peak(c)end end
 local low=dangerousFocus or p:getHealth()<65 or(p:getHealth()/p:getMaxHealth()<.5 and #adj>1)or(#adj>0 and burst*2>p:getHealth()-25)
 local committedRat=self:ratTarget(p,threats)
 if committedRat then low=false end
 if low then self.resumeGoal=nil end
 if low and(not self.goal or(#adj>0 and(self.goal.kind=='RECOVER'or self.goal.kind=='RETURN'))or(self.goal.kind~='RECOVER'and self.goal.kind~='RETURN'and self.goal.kind~='ESCAPE'and self.goal.kind~='RETREAT_ACCESS'and(self.goal.kind~='WAIT'or #adj>0)))then
  local g
  if dangerousFocus or #adj>0 then g=self:returnExit(p)or self:escapeGoal(p,threats)or{id='WAIT',kind='WAIT',goal=cp(here),score=-10}
  elseif p:getHealth()<65 and self.memory:allowed('RECOVER:Cipfried')then g={id='RECOVER:Cipfried',kind='RECOVER',goal=self.cfg.temple,score=100}
  elseif #threats==0 and fed(p)and not p:getTile():hasFlag(TILESTATE_PROTECTIONZONE)then g={id='REST:'..Autonomy.region(here),kind='REST',goal=cp(here),score=100}
  else g=self:escapeGoal(p,threats)or{id='WAIT',kind='WAIT',goal=cp(here),score=-10}end
  if not(self.goal and self.goal.kind=='WAIT'and g.kind=='WAIT')then
   if self.goal then self:finish(p,'UNCERTAIN','SAFETY_RECONSIDERATION',{g})end
   self.policy:choose(self:state(p),{g});g.at=now();g.xp=xp;g.lastUseful=now();self.goal=g
   if g.kind=='ESCAPE'then self.emit('LOCAL_ESCAPE_SELECTED',{position=g.goal,hp=p:getHealth()})end
  end
 end
 if committedRat then
  if self.goal and self.goal.kind=='HUNT'and self.goal.target==committedRat:getId()then self.goal.ratLocked=true
  else
   local q=cp(committedRat:getPosition());local previous=self.goal
   local g={id='FINISH:'..committedRat:getName():lower()..':'..committedRat:getId(),kind='HUNT',ratLocked=true,incidental=true,target=committedRat:getId(),goal=q,score=200,at=now(),xp=xp,progress=now(),lastUseful=now()}
   if self.goal then self:finish(p,'UNCERTAIN','RAT_FINISH_PRIORITY',{g})end
   self.resumeGoal=previous;self.policy:choose(self:state(p),{g});self.goal=g
   self.emit('RAT_TARGET_LOCKED',{target=g.target,targetHp=committedRat:getHealth(),hp=p:getHealth()})
  end
 end
 local finisher=self:engagedTarget(p,low)
 if finisher then
  self.resumeGoal=self.goal
  if self.goal then self:finish(p,'UNCERTAIN','FINISH_ENGAGED_TARGET',{finisher})end
  self.policy:choose(self:state(p),{finisher});self.goal=finisher
  self.emit('ENGAGED_TARGET_FINISH_STARTED',{target=finisher.target,species=Creature(finisher.target):getName(),hp=p:getHealth()})
 end
 if p:getLevel()>=3 and not low and self.goal and(self.goal.kind=='SECTOR'or self.goal.kind=='EXPLORE')then
  for _,c in ipairs(threats)do
   if self:safeTarget(p,c)and distance(here,c:getPosition())<=6 then
    local plan=self.router.nav:plan(here,c:getPosition(),{preview=true,adjacent=true,limit=500,radius=12})
    if plan.status=='ROUTE'or plan.status=='REACHED'then
     local pos=cp(c:getPosition());local g={id='HUNT:'..c:getName():lower()..'@'..math.floor(pos.x/8)..','..math.floor(pos.y/8)..','..pos.z,kind='HUNT',target=c:getId(),goal=pos,score=30,at=now(),xp=xp,progress=now(),lastUseful=now()}
     self:finish(p,'COMPLETED','PREFERRED_TARGET_FOUND',{g});self.policy:choose(self:state(p),{g});self.goal=g
     self.emit('PREFERRED_HUNT_STARTED',{species=c:getName(),level=p:getLevel(),position=pos});break
    end
   end
  end
 end
 if self.goal and self.goal.kind~='HUNT'then
  for _,c in ipairs(adj)do if here:isSightClear(c:getPosition(),true)then p:setTarget(c);break end end
 end
 if not self.goal or self.goal.kind~='HUNT'then p:setFollowCreature(nil)end
 if #threats>0 and self.scavengeJob then Scavenge.release(self,'THREAT_VISIBLE')end
 if not self.goal and Scavenge.tick(self,p,threats)then return end
 if not self.goal and self:loot(p)then return end
 if self.resumeGoal and self.progression and not self.progression:resumeAllowed(self.resumeGoal)then self.emit('QUEST_RESUME_DEFERRED',{id=self.resumeGoal.id});self.resumeGoal=nil end
 if not self.goal and self.resumeGoal then
  local previous=self.resumeGoal;self.resumeGoal=nil;previous.at=now();previous.lastUseful=now();previous.xp=xp
  self.policy:choose(self:state(p),{previous});self.goal=previous;self:reserveSector(previous)
  if self.progression then self.progression:onChosen(p,previous)end
  self.emit('PREVIOUS_GOAL_RESUMED',{id=previous.id})
 end
 if self.policy.pollFeedback and self.policy:pollFeedback()and self.goal then self:finish(p,'REJECTED','TIBINHA_CLICK_WRONG')end
 if not self.goal then
  self.inventory:discard(p,self.emit)
  local goals=self:candidates(p);local g,random,q=self.policy:choose(self:state(p),goals);g.at=now();g.xp=xp;g.progress=now();g.lastUseful=now();self.goal=g;self:reserveSector(g)
  if g.kind=='SECTOR'and p:getLevel()>=3 then self.emit('KNOWN_HUNT_SPOT_SELECTED',{species=g.sectorName or g.id,position=g.goal,level=p:getLevel()})end
  if self.progression then self.progression:onChosen(p,g)end
  self.emit('META_CHOSEN',{id=g.id,kind=g.kind,q=q,exploratory=random,routeStatus=g.routeStatus or'SAFETY',regionValue=self.memory:value(here),position=cp(here)})
 end
 if not self.goal.ratLocked and self.goal.kind~='REST'and self.goal.kind~='RECOVER'and self.goal.kind~='WAIT'and now()-(self.goal.lastUseful or self.goal.at)>45000 then
  self.stats.loops=self.stats.loops+1;self:finish(p,'UNCERTAIN','NO_USEFUL_PROGRESS');return
 end
 if not self.goal.ratLocked and now()-self.goal.at>150000 then self:finish(p,'UNCERTAIN','META_TIME_LIMIT');return end
 self:execute(p)
 self.router.fastOwner=self.goal
end
function Brain:save(p,terminal)
 if terminal then CromeCaveAssignments[self.a.name]=nil end
 if terminal and self.goal then local lease=CromeSectorReservations[self.goal.id];if lease and lease.owner==self.a.name then CromeSectorReservations[self.goal.id]=nil end end
 if terminal then Scavenge.release(self,'SESSION_END')end
 if not CromeDangerSavedAt or now()-CromeDangerSavedAt>15000 then
  local rows={};for _,spot in pairs(CromeDangerSpots)do if spot.untilAt>os.time()then rows[#rows+1]=table.concat({spot.pos.x,spot.pos.y,spot.pos.z,spot.name,spot.untilAt},'\t')end end
  assert(self.router.nav.atomic(self.cfg.memoryRoot..'/danger_spots.tsv',table.concat(rows,'\n')..'\n'));CromeDangerSavedAt=now()
 end
 self:flushMotion()
 if terminal and self.policy.pending then
  local r=self.policy:finish('UNCERTAIN',self:state(p),{},true);self.emit('META_CENSORED',{reason='SESSION_END',reward=r.reward,updates=r.updates});self.goal=nil
 end
 if self.progression then local saved,why=self.progression:save();assert(saved,tostring(why))end
 local ok,err=self.memory:save();assert(ok,tostring(err));ok,err=self.router:save();assert(ok,tostring(err));ok,err=self.policy:save();assert(ok,tostring(err))
 local keys={};for k in pairs(self.stats)do keys[#keys+1]=k end;table.sort(keys);local lines={'CROME_RACE_STATS_1\t'..self.cfg.domain};for _,k in ipairs(keys)do lines[#lines+1]=k..'\t'..math.floor(self.stats[k])end
 assert(self.router.nav.atomic(self.stem..'_stats.tsv',table.concat(lines,'\n')..'\n'))
end
Brain.Market=Market
return Brain
