local Autonomy=dofile('data/crome_race/autonomy.lua')
local Policy=dofile('data/crome_race/policy.lua')
local Router=dofile('data/crome_race/router.lua')
local Market=dofile('data/crome_race/market.lua')
local Brain={};Brain.__index=Brain
local function now()return os.mtime()end
local function cp(p)return Router.copy(p)end
local function key(p)return Router.key(p)end
local function distance(a,b)return Router.distance(a,b)end
local function fed(p)local c=p:getCondition(CONDITION_REGENERATION,CONDITIONID_DEFAULT);return c and c:getTicks()>10000 end
local tier={rat=1,['cave rat']=1,bug=1,spider=1,wolf=2,snake=2,troll=2,skeleton=3,orc=3,bear=3}
function Brain.new(a,cfg,wiki,prior,inventory,emit)
 local self=setmetatable({a=a,cfg=cfg,wiki=wiki,inventory=inventory,emit=emit,failed={},failureCounts={},lastXp=0,lastHp=0,loots={},recent={},lastCheck=now(),lastXpAt=now(),floorEnteredAt=now()},Brain)
 a.spawn=cp(Player(a.id):getPosition());self.router=Router.new(a,wiki,prior,cfg,inventory,emit)
 local stem=cfg.memoryRoot..'/'..a.name:gsub('[^%w_]','_')
 self.policy=Policy.new{domain=cfg.domain,path=stem..'_policy.tsv',atomic=self.router.nav.atomic,seed=a.index*7919+17,priorWeight=.25}
 local p=Player(a.id);self.lastXp=p:getExperience();self.lastHp=p:getHealth();self.stats={played=0,deaths=0,earned=0,spent=0,moves=0,transitions=0,loops=0,metas=0,uncertain=0}
 self.stem=stem
 self.memory=Autonomy.new{domain=cfg.domain,path=stem..'_autonomy.tsv',atomic=self.router.nav.atomic}
 self.memory:arrive(p:getPosition());self.lastRegion=Autonomy.region(p:getPosition());self.lastUseful=now();self.lastMoney=p:getMoney()
 self.selectionStats={filtered=0};self.lastFilterLog={}

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
function Brain:safeTarget(p,c)
 local name=c:getName():lower();local info=self.wiki.monsters[name];local level=tier[name]or 10
 if not info or level>p:getLevel()then return false end
 if info.poison and(not fed(p)or p:getHealth()/p:getMaxHealth()<.85)then return false end
 local focus=c:getTarget();if focus and focus:isPlayer()and focus:getId()~=p:getId()and distance(p:getPosition(),c:getPosition())>1 then return false end
 return p:getHealth()>self:peak(c)*3+35
end
function Brain:candidates(p)
 local list={};local here=p:getPosition();local health=p:getHealth()/p:getMaxHealth();local inv=self.inventory;local enemies=self:enemies(p)
 local function add(g,score)
  if (self.failed[g.id]and self.failed[g.id]>now())or not self.memory:allowed(g.id)then return end
  local regional=g.goal and self.memory:value(g.goal)or 0
  g.score=score+((g.kind=='HUNT'or g.kind=='SECTOR'or g.kind=='EXPLORE'or g.kind=='ENTER')and regional or 0);list[#list+1]=g
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
 if health<.6 and fed(p)then
  local rest=(#enemies==0 and not p:getTile():hasFlag(TILESTATE_PROTECTIONZONE))and cp(here)or{x=32097,y=32212,z=7}
  add({id='REST:'..Autonomy.region(rest),kind='REST',goal=rest},30)
 end
 local upgrade=inv:upgrade(p)
 if upgrade then add({id='EQUIP:'..upgrade.row.id,kind='EQUIP',upgrade=upgrade},40)end
 for _,sale in ipairs(inv:sellOptions(p))do
  if sale.count>=2 or not inv:destination(p,sale.item)or p:getFreeCapacity()<5000 then
   sale.id='SELL:'..sale.name..'@'..sale.vendor.name;sale.goal=sale.vendor.pos;add(sale,6+math.min(4,sale.count*.2)-distance(here,sale.goal)*.002)
  end
 end
 for _,buy in ipairs(inv:buyOptions(p))do buy.id='BUY:'..buy.name..'@'..buy.vendor.name;buy.goal=buy.vendor.pos;add(buy,4+math.min(4,buy.gain/(buy.price+1)*4))end
 for _,c in ipairs(enemies)do
  if self:safeTarget(p,c)then local pos=c:getPosition();local plan=self.router.nav:plan(here,pos,{adjacent=true,limit=700,radius=12})
   if plan.status=='ROUTE'or plan.status=='REACHED'then
    local info=self.wiki.monsters[c:getName():lower()];add({id='HUNT:'..c:getName():lower()..'@'..math.floor(pos.x/8)..','..math.floor(pos.y/8)..','..pos.z,kind='HUNT',target=c:getId(),goal=cp(pos)},12+info.experience*.03-distance(here,pos)*.05)
   end
  end
 end
 if health>=.6 then
  if here.z==7 then
   local peers=Game.getSpectators(here,false,true,7,7,5,5)
   for _,entry in ipairs(self.wiki.entrances)do
    local ref=nil;for _,v in ipairs(self.wiki.accesses)do if key(v.pos)==key(entry.pos)and v.id==entry.id then ref=v;break end end
    if ref then
     local congestion=0;local vx,vy=ref.pos.x-here.x,ref.pos.y-here.y
     for _,peer in ipairs(peers)do if peer:getId()~=p:getId()then local q=peer:getPosition();if(q.x-here.x)*vx+(q.y-here.y)*vy>0 then congestion=congestion+1 end end end
     add({id='ENTER:'..key(ref.pos),kind='ENTER',access=ref,goal=ref.pos},4-distance(here,ref.pos)*.025-congestion*.16+self.policy:random()*1.1)
    end
   end
  else
   local spots={}
   for _,spawn in ipairs(self.wiki.spawns)do if spawn.pos.z==here.z and(tier[spawn.name]or 10)<=p:getLevel()and self.memory:allowed('SECTOR:'..spawn.name..'@'..math.floor(spawn.pos.x/8)..','..math.floor(spawn.pos.y/8)..','..spawn.pos.z)then spots[#spots+1]={spawn=spawn,d=distance(here,spawn.pos)}end end
   table.sort(spots,function(a,b)return a.d<b.d end)
   for i=1,math.min(5,#spots)do local s=spots[i].spawn;local region=math.floor(s.pos.x/8)..','..math.floor(s.pos.y/8)..','..s.pos.z
    if distance(here,s.pos)>2 then add({id='SECTOR:'..s.name..'@'..region,kind='SECTOR',goal=s.pos,revisitSeconds=s.seconds or 60},3-spots[i].d*.02+(self.wiki.monsters[s.name].experience or 0)*.02)end
   end
   if #enemies==0 and now()-math.max(self.lastXpAt,self.floorEnteredAt)>45000 then add({id='RETURN:city',kind='RETURN',goal=self.cfg.temple},3)end
   -- A deeper sewer is considered only after its physical item is visible.
   for _,ref in ipairs(self.wiki.accesses)do if ref.pos.z==here.z and ref.delta==1 and ref.id==430 and distance(here,ref.pos)<=6 then add({id='ENTER:'..key(ref.pos),kind='ENTER',access=ref,goal=ref.pos},4)end end
  end
 end
 local frontiers=self.router.nav:frontiers(here,self.a.spawn,100)
 for i=1,math.min(3,#frontiers)do local f=frontiers[i];add({id='EXPLORE:'..key(f.goal),kind='EXPLORE',goal=f.goal},1+f.score*.05)end
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
 table.sort(list,function(a,b)return self.policy:value(self:state(p),a.id)+a.score*.25>self.policy:value(self:state(p),b.id)+b.score*.25 end)
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
 if #reachable==0 then reachable[1]={id='WAIT',kind='WAIT',goal=cp(here),score=-10}end
 return reachable
end
function Brain:finish(p,status,reason)
 if not self.policy.pending then return end
 local goal=self.goal;if goal then self.memory:outcome(goal.id,status,reason)end;if self.a.trade then Market.release(self.a,p)end;local reward=self.policy:finish(status,self:state(p),self:candidates(p),false)
 if status=='COMPLETED'then self.stats.metas=self.stats.metas+1 else self.stats.uncertain=self.stats.uncertain+1 end
 self.emit('META_RESULT',{id=goal and goal.id or'?',status=status,reason=reason,reward=reward.reward,q=reward.value,confirmedSteps=reward.confirmed,updates=reward.updates})
 if status~='COMPLETED'and goal then
  local count=(self.failureCounts[goal.id]or 0)+1;self.failureCounts[goal.id]=count
  self.failed[goal.id]=now()+math.min(300000,20000*2^(math.min(count,5)-1))
 elseif goal then self.failureCounts[goal.id]=nil end
 if goal and goal.kind=='ENTER'and status=='COMPLETED'then self.failed[goal.id]=now()+45000 end
 if goal and goal.kind=='SECTOR'and status=='COMPLETED'then self.memory:pause(goal.id,math.max(30,goal.revisitSeconds or 60),'SECTOR_SCOUTED')end
 if goal and goal.kind=='EXPLORE'and status=='COMPLETED'then self.memory:pause(goal.id,120,'ALREADY_EXPLORED')end
 self.goal=nil;self.blockedAt=nil;self.router.path=nil;self.router.pathGoal=nil
end
function Brain:travel(p,goal,adjacent,returning)
 local status=self.router:go(p,goal,adjacent,returning)
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
  g.goal=cp(target:getPosition());p:setTarget(target)
  if now()-(g.progress or g.at)>18000 then p:setTarget(nil);self:finish(p,'UNCERTAIN','TARGET_NO_PROGRESS');return end
  if distance(here,g.goal)>1 then self:travel(p,g.goal,true)end
 elseif g.kind=='ENTER'then
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
    self.emit('QUEST_REWARD_CONFIRMED',{uid=g.uid,position=g.goal,storage=p:getStorageValue(g.uid)})
    self.policy:reward('QUEST_DISCOVERY',2);self:finish(p,'COMPLETED','QUEST_STORAGE_OBSERVED');return
   end
   if now()-g.sentAt>3000 then local id=g.id;self:finish(p,'UNCERTAIN','QUEST_NO_OBSERVED_CHANGE');self.memory:pause(id,300,'QUEST_NO_OBSERVED_CHANGE')end
  else
   g.beforeStorage=p:getStorageValue(g.uid)
   if p:cromeUseObject(chest)then g.sentAt=now();self.emit('QUEST_USE_SENT',{uid=g.uid,position=g.goal})end
  end
 elseif g.kind=='EQUIP'then
  local up=g.upgrade;local expected=up.row.id;local worn=p:getSlotItem(up.slot)
  if worn and worn:getId()==expected then self.policy:reward('EQUIPMENT_GAIN',up.gain*.15);self:finish(p,'COMPLETED','EQUIPMENT_OBSERVED');return end
  local ok,why=p:cromeMoveOwned(up.row.ref,up.slot)
  if not ok and why~='COOLDOWN'then self:finish(p,'REJECTED','EQUIP_REJECTED')end
 elseif g.kind=='BUY'or g.kind=='SELL'then
  local npcPosition=g.vendor.pos
  for _,c in ipairs(Game.getSpectators(here,false,false,7,7,5,5))do
   if c:isNpc()and c:getName():lower()==g.vendor.name:lower()then npcPosition=cp(c:getPosition());break end
  end
  if self:travel(p,npcPosition,true)~='ARRIVED'then return end
  local status,money=Market.tick(self.a,p,g,self.emit)
  if status=='COMPLETED'then
   if money>0 then self.stats.earned=self.stats.earned+money;self.policy:reward('SALE_INCOME',money*.05)else self.stats.spent=self.stats.spent-money;self.policy:reward('PURCHASE_COST',money*.01);self.policy:reward('UPGRADE_VALUE',(g.gain or 0)*.15)end
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
  local cip=nil;for _,v in ipairs(self.wiki.vendors)do if v.name:lower()=='cipfried'then cip=v;break end end
  if not cip then self:finish(p,'UNCERTAIN','HEALER_UNKNOWN');return end
  if self:travel(p,cip.pos,true,true)~='ARRIVED'then return end
  local status=Market.tick(self.a,p,{kind='HEAL',vendor=cip},self.emit)
  if status=='COMPLETED'then self:finish(p,'COMPLETED','HP_RECOVERY_OBSERVED')elseif status=='UNCERTAIN'then self:finish(p,'UNCERTAIN','NPC_HEAL_UNCONFIRMED')end
 elseif g.kind=='REST'then
  if self:travel(p,g.goal,false,true)~='ARRIVED'then return end
  if p:getTile():hasFlag(TILESTATE_PROTECTIONZONE)then self:finish(p,'UNCERTAIN','NO_REGEN_IN_PZ');return end
  if not fed(p)or p:getHealth()/p:getMaxHealth()>=.65 then self:finish(p,'COMPLETED','REST_BOUNDARY');return end
 elseif g.kind=='SWITCH'then
  if g.object:getId()==1946 then local id=g.id;self:finish(p,'COMPLETED','SHARED_SWITCH_OPEN');self.failed[id]=now()+90000;return end
  local before=g.object:getId();local ok=p:cromeUseObject(g.object)
  if ok and g.object:getId()~=before then self.emit('SWITCH_STATE_OBSERVED',{pos=key(g.goal),id=g.object:getId()});self.policy:reward('INTERACTION_OBSERVED',.2);self:finish(p,'COMPLETED','SWITCH_CHANGED')end
 elseif g.kind=='WAIT'then if now()-g.at>1500 then self:finish(p,'UNCERTAIN','WAIT_BOUNDARY')end
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
function Brain:tick(p)
 local elapsed=math.min(2000,now()-self.lastCheck);self.lastCheck=now();self.stats.played=self.stats.played+elapsed
 local xp=p:getExperience();local hp=p:getHealth();local gain=xp-self.lastXp;local lost=self.lastHp-hp;self.lastXp=xp;self.lastHp=hp
 if gain~=0 then self.lastXpAt=now();self.emit('XP_CHANGED',{delta=gain,xp=xp,level=p:getLevel()});if self.policy.pending then self.policy:reward('XP',gain*.2)end end
 if lost>0 and self.policy.pending then self.policy:reward('HP_LOSS',-lost*.08)end
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
  if observed.kind=='ACCESS'then self.floorEnteredAt=now();self.stats.transitions=self.stats.transitions+1;if self.policy.pending then self.policy:reward('ACCESS_OBSERVED',.5)end;if self.goal and self.goal.kind=='ENTER'then self:finish(p,'COMPLETED','FLOOR_OBSERVED')end
  elseif not observed.kind then self.stats.moves=self.stats.moves+1 end
 elseif observed and observed.status=='UNCERTAIN'and self.goal then self:finish(p,'UNCERTAIN',observed.reason)end
 self.router:observe(p);self.inventory:feed(p,self.a,self.emit)
 local here=p:getPosition();local threats=self:enemies(p);local adj={};local burst=0;local dangerousFocus=false
 for _,c in ipairs(threats)do
  local focus=c:getTarget();local dangerous=(tier[c:getName():lower()]or 10)>p:getLevel()or self:peak(c)*3+35>=p:getHealth()
  if dangerous then
   local q=c:getPosition()
   for dx=-1,1 do for dy=-1,1 do local avoid={x=q.x+dx,y=q.y+dy,z=q.z}
    if key(avoid)~=key(here)then self.router.nav:observe(avoid,false,'THREAT')end
   end end
   if focus and focus:getId()==p:getId()then dangerousFocus=true end
  end
 end
 for _,c in ipairs(threats)do if distance(here,c:getPosition())<=2 then adj[#adj+1]=c;burst=burst+self:peak(c)end end
 local low=dangerousFocus or p:getHealth()<65 or(p:getHealth()/p:getMaxHealth()<.5 and #adj>1)or(#adj>0 and burst*2>p:getHealth()-25)
 if low and(not self.goal or(self.goal.kind~='RECOVER'and self.goal.kind~='RETURN'and self.goal.kind~='ESCAPE'and(self.goal.kind~='WAIT'or #adj>0)))then
  local g
  if self.memory:allowed('RECOVER:Cipfried')then g={id='RECOVER:Cipfried',kind='RECOVER',goal=self.cfg.temple,score=100}
  elseif #threats==0 and fed(p)and not p:getTile():hasFlag(TILESTATE_PROTECTIONZONE)then g={id='REST:'..Autonomy.region(here),kind='REST',goal=cp(here),score=100}
  else g=self:escapeGoal(p,threats)or{id='WAIT',kind='WAIT',goal=cp(here),score=-10}end
  if not(self.goal and self.goal.kind=='WAIT'and g.kind=='WAIT')then
   if self.goal then self:finish(p,'UNCERTAIN','SAFETY_RECONSIDERATION')end
   self.policy:choose(self:state(p),{g});g.at=now();g.xp=xp;g.lastUseful=now();self.goal=g
   if g.kind=='ESCAPE'then self.emit('LOCAL_ESCAPE_SELECTED',{position=g.goal,hp=p:getHealth()})end
  end
 end
 if self.goal and self.goal.kind~='HUNT'then
  for _,c in ipairs(adj)do if here:isSightClear(c:getPosition(),true)then p:setTarget(c);break end end
 end
 if not self.goal and self:loot(p)then return end
 if not self.goal then
  self.inventory:discard(p,self.emit)
  local goals=self:candidates(p);local g,random,q=self.policy:choose(self:state(p),goals);g.at=now();g.xp=xp;g.progress=now();g.lastUseful=now();self.goal=g
  self.emit('META_CHOSEN',{id=g.id,kind=g.kind,q=q,exploratory=random,routeStatus=g.routeStatus or'SAFETY',regionValue=self.memory:value(here),position=cp(here)})
 end
 if self.goal.kind~='REST'and self.goal.kind~='RECOVER'and self.goal.kind~='WAIT'and now()-(self.goal.lastUseful or self.goal.at)>45000 then
  self.stats.loops=self.stats.loops+1;self:finish(p,'UNCERTAIN','NO_USEFUL_PROGRESS');return
 end
 if now()-self.goal.at>150000 then self:finish(p,'UNCERTAIN','META_TIME_LIMIT');return end
 self:execute(p)
end
function Brain:save(p,terminal)
 if terminal and self.policy.pending then
  local r=self.policy:finish('UNCERTAIN',self:state(p),{},true);self.emit('META_CENSORED',{reason='SESSION_END',reward=r.reward,updates=r.updates});self.goal=nil
 end
 local ok,err=self.memory:save();assert(ok,tostring(err));ok,err=self.router:save();assert(ok,tostring(err));ok,err=self.policy:save();assert(ok,tostring(err))
 local keys={};for k in pairs(self.stats)do keys[#keys+1]=k end;table.sort(keys);local lines={'CROME_RACE_STATS_1\t'..self.cfg.domain};for _,k in ipairs(keys)do lines[#lines+1]=k..'\t'..math.floor(self.stats[k])end
 assert(self.router.nav.atomic(self.stem..'_stats.tsv',table.concat(lines,'\n')..'\n'))
end
Brain.Market=Market
return Brain
