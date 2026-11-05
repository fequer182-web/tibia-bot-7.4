-- Advisory progression: goals guide learning; they never freeze XP or require an exact item.
CromeQuestLeases=CromeQuestLeases or{}
local P={};P.__index=P
local function clock()return os.mtime()end
local function position(q)return{x=q.x,y=q.y,z=q.z}end
local function region(q)return q.z..':'..math.floor(q.x/32)..','..math.floor(q.y/32)end
local targets={leather={{1,2461},{4,2467},{7,2649},{8,2643},{5,2512}},studded={{1,2482},{4,2484},{7,2468},{8,2643},{5,2512},{6,2398}}}
local quests={{uid=7013,itemId=1747,reward=2404,pos={x=32102,y=32235,z=8}},{uid=7023,itemId=1740,reward=2384,pos={x=32099,y=32198,z=9}}}
function P.new(brain)
 local self=setmetatable({brain=brain,records={},cooldown={},questFailures={},kills=0,improvements=0,lastStatusAt=0},P)
 local f=io.open(brain.stem..'_progression.tsv','r')
 if f then
  if f:read('*l')=='CROME_PROGRESSION_1\t'..brain.cfg.domain then
   for line in f:lines()do
    local key,n,xp,damage,seconds,bad=line:match('^R\t([^\t]+)\t(%d+)\t([%d%.%-]+)\t([%d%.]+)\t([%d%.]+)\t(%d+)$')
    if key then self.records[key]={n=tonumber(n),xp=tonumber(xp),damage=tonumber(damage),seconds=tonumber(seconds),bad=tonumber(bad)}end
    local uid,untilAt,links=line:match('^Q\t(%d+)\t(%d+)\t(%d+)$');if uid then self.questFailures[tonumber(uid)]={untilAt=tonumber(untilAt),links=tonumber(links)}end
    local k,v=line:match('^C\t(%w+)\t(%d+)$');if k and(k=='kills'or k=='improvements')then self[k]=tonumber(v)end
   end
  end
  f:close()
 end
 return self
end
function P:profile(p)
 local inv=self.brain.inventory;local equipped={};local armor=0;local shield=0;local weapon=0
 for _,slot in ipairs({1,4,5,6,7,8})do
  local item=p:getSlotItem(slot);local info=item and self.brain.wiki.items[item:getId()]or{}
  equipped[tostring(slot)]=item and item:getId()or 0;armor=armor+(info.armor or 0)
  if slot==5 then shield=info.weapon=='shield'and(info.defense or 0)or 0 elseif slot==6 then weapon=inv:utility(p,info,6)end
 end
 return{equipped=equipped,armor=armor,shield=shield,weaponUtility=weapon,key=p:getLevel()..':'..armor..':'..shield..':'..math.floor(weapon)}
end
function P:needs(p)
 local name=p:getLevel()>=3 and'studded'or'leather';local out={};local inv=self.brain.inventory
 for _,t in ipairs(targets[name])do
  local slot,id=t[1],t[2];local item=p:getSlotItem(slot);local old=item and self.brain.wiki.items[item:getId()]or{};local info=self.brain.wiki.items[id]or{}
  local gap=inv:utility(p,info,slot)-inv:utility(p,old,slot)
  if gap>0 then out[#out+1]={slot=slot,referenceId=id,gap=gap}end
 end
 table.sort(out,function(a,b)return a.gap>b.gap end);return out,name
end
function P:ready(p)
 local a=self:profile(p)
 return p:getLevel()>=3 and a.armor>=6 and a.shield>0 and a.weaponUtility>=10
end
function P:advanced(q)
 return q.z>=6 and q.z<=7 and q.x>=31988 and q.x<=32031 and q.y>=32198 and q.y<=32249
end
function P:allow(p,g)
 if g.kind~='ENTER'and g.kind~='SECTOR'and g.kind~='EXPLORE'and g.kind~='CIRCUIT'then return true end
 local q=g.goal;if not q then return true end
 local cooldown=self.cooldown[region(q)];if cooldown and cooldown>clock()then return false,'AREA_TRIAL_UNPROFITABLE'end
 if self:advanced(q)then
  if not self:ready(p)or p:getHealth()/p:getMaxHealth()<.8 then return false,'ADVANCED_AREA_NEEDS_GEAR'end
  if g.kind=='ENTER'and g.access and g.access.delta==1 then
   if p:getItemCount(2120)==0 or p:getItemCount(2554)==0 then return false,'ADVANCED_DESCENT_NEEDS_TOOLS'end
   -- Tools alone do not prove the Lua/native access executor supports this hole.
   if not(self.brain.cfg.validatedToolDescents and self.brain.cfg.validatedToolDescents[self.brain.router.key(g.access.pos)])then return false,'TOOL_DESCENT_NOT_VALIDATED'end
  end
 end
 return true
end
function P:purchaseWorthTrip(p,g)
 local here=p:getPosition();local q=g.vendor.pos
 local localShop=here.z==q.z and math.abs(here.x-q.x)+math.abs(here.y-q.y)<=12
 local slot=self.brain.inventory:slot(self.brain.wiki.items[g.item]or{})
 local criticalEmpty=slot and not p:getSlotItem(slot)and(slot==4 or slot==5 or slot==6)
 return localShop or criticalEmpty or(g.gain or 0)>=4 or p:getMoney()>=(self.brain.cfg.shoppingTripMinimum or 30)or g.item==1988
end
function P:ratFallback(p)return(self.fallbackUntil or 0)>clock()end
function P:protectGoal(g)
 return g and(g.kind=='BUY'or g.kind=='SELL'or g.kind=='EQUIP')
end
function P:focus(p,list)
 local equip={};for _,g in ipairs(list)do if g.kind=='EQUIP'then equip[#equip+1]=g end end
 if #equip>0 then return equip end
 if p:getHealth()/p:getMaxHealth()<.7 or self.brain:unsafeGoal(p,p:getPosition())then return list end
 local needs=self:needs(p);local slots={};for _,n in ipairs(needs)do slots[n.slot]=true end
 local buys={}
 for _,g in ipairs(list)do
  local slot=g.kind=='BUY'and self.brain.inventory:slot(self.brain.wiki.items[g.item]or{})
  if g.kind=='BUY'and(slots[slot]or g.item==1988)then g.procurement=true;buys[#buys+1]=g end
 end
 if #buys>0 then return buys end
 return list
end
function P:purchaseScore(p,g,base)
 local inv=self.brain.inventory;local needs=self:needs(p);local slot=inv:slot(self.brain.wiki.items[g.item]or{});local wanted=false
 for _,n in ipairs(needs)do if n.slot==slot then wanted=true;break end end
 local here=p:getPosition();local q=g.vendor.pos;local travel=math.abs(here.x-q.x)+math.abs(here.y-q.y)+math.abs(here.z-q.z)*20
 local score=base+math.min(5,(g.gain or 0)/math.max(1,g.price)*10)-travel*.015
 if wanted then
  score=score+14
  for _,n in ipairs(needs)do if n.slot==slot then
   local offered=self.brain.wiki.items[g.item]or{};local target=self.brain.wiki.items[n.referenceId]or{}
   if inv:utility(p,offered,slot)>=inv:utility(p,target,slot)then score=score+6 end
  end end
 end
 if g.item==1988 then score=score+8 end
 -- A soft reserve for future equipment: never blocks an essential upgrade or hunting.
 if not wanted and p:getMoney()-g.price<(self.brain.cfg.progressionReserveGold or 20)then score=score-6 end
 for _,qg in ipairs(self.questOptions or{})do
  if qg.rewardItem==g.item then score=score-12;break end
 end
 return score
end
function P:linkCount()
 local n=0;for _ in pairs(self.brain.router.links or{})do n=n+1 end;return n
end
function P:questPermit(uid)
 local failed=self.questFailures[uid]
 if failed and failed.untilAt>clock()and self:linkCount()<=failed.links then return false end
 local key=uid..':'..(self.brain.a.variant or'unknown');local leases=CromeQuestLeases[key]or{};CromeQuestLeases[key]=leases
 local count=0
 for owner,untilAt in pairs(leases)do if untilAt<=clock()then leases[owner]=nil elseif owner~=self.brain.a.name then count=count+1 end end
 return count<(self.brain.cfg.questConcurrencyPerArm or 2)
end
function P:leaseQuest(g)
 if not g or not g.progressionQuest then return end
 local key=g.uid..':'..(self.brain.a.variant or'unknown');CromeQuestLeases[key]=CromeQuestLeases[key]or{};CromeQuestLeases[key][self.brain.a.name]=clock()+15000
end
function P:resumeAllowed(g)return not g.progressionQuest or self:questPermit(g.uid)end
function P:questGoals(p)
 local b=self.brain;local inv=b.inventory;local here=p:getPosition();local out={};local old=p:getSlotItem(6);local oldInfo=old and b.wiki.items[old:getId()]or{}
 if p:getHealth()/p:getMaxHealth()<.8 then self.questOptions=out;return out end
 for _,q in ipairs(quests)do
  local id='INSPECT:'..q.uid..'@'..b.router.key(q.pos);local info=b.wiki.items[q.reward]or{};local gain=inv:utility(p,info,6)-inv:utility(p,oldInfo,6)
  local dist=math.abs(here.x-q.pos.x)+math.abs(here.y-q.pos.y);local floorGap=math.abs(here.z-q.pos.z);local returnRef=here.z==q.pos.z and(here.z<=7 or b.router.entry[here.z]~=nil)
  if not returnRef and floorGap==1 then
   for _,ref in ipairs(b.wiki.accesses)do
    if ref.pos.z==q.pos.z and ref.delta==(here.z>q.pos.z and 1 or -1)and math.abs(ref.pos.x-q.pos.x)+math.abs(ref.pos.y-q.pos.y)<=35 and(ref.id==1386 or ref.id==1385 or ref.id==423)then returnRef=true;break end
   end
  end
  local shield=p:getSlotItem(5)
  if self:questPermit(q.uid)and gain>0 and p:getStorageValue(q.uid)<1 and p:getItemCount(q.reward)==0 and dist<=80 and floorGap<=1 and returnRef and(q.uid~=7023 or(shield and(b.wiki.items[shield:getId()]or{}).weapon=='shield'))and inv:destination(p,q.reward)and p:getFreeCapacity()>=(info.weight or 0)and(b.failed[id]or 0)<=clock()and b.memory:allowed(id)and not b:unsafeGoal(p,q.pos)then
   out[#out+1]={id=id,kind='INSPECT',goal=position(q.pos),uid=q.uid,itemId=q.itemId,rewardItem=q.reward,progressionQuest=true,score=22+math.min(3,gain)-dist*.08-floorGap*3,returnStatus=here.z==7 and'SURFACE'or'REFERENCE_REQUIRES_LIVE_CONFIRMATION'}
  end
 end
 self.questOptions=out;return out
end
function P:toolGoals(p)
 if not self.brain.cfg.validatedToolDescents or not next(self.brain.cfg.validatedToolDescents)then return{}end
 local needs=self:needs(p);if #needs>0 or not self:ready(p)then return{}end
 local out={};local inv=self.brain.inventory
 for _,v in ipairs(self.brain.wiki.vendors)do for _,o in ipairs(v.buy)do
  if(o.id==2120 or o.id==2554)and p:getItemCount(o.id)==0 and o.price<=p:getMoney()and inv:destination(p,o.id)then out[#out+1]={kind='BUY',vendor=v,item=o.id,name=o.name,price=o.price,gain=2}end
 end end
 return out
end
function P:areaScore(p,g,base)
 local r=self.records[region(g.goal)..'|'..self:profile(p).key]
 if r and r.n>0 then return base+math.min(3,r.xp/math.max(1,r.seconds)*10)-math.min(12,r.bad/r.n*12)end
 return base
end
function P:onChosen(p,g)
 self:leaseQuest(g)
 if self:protectGoal(g)then self.brain.emit('PROCUREMENT_COMMITTED',{id=g.id,kind=g.kind,item=g.item,price=g.price,slot=g.slot})end
 if(g.kind=='SECTOR'or g.kind=='CIRCUIT')and self:ready(p)then self.trialGoal={pos=position(g.goal),goalId=g.id,profile=self:profile(p).key,selectedAt=clock()}end
end
function P:evaluate(p,reason)
 local t=self.trial;if not t then return end
 local seconds=math.max(1,(clock()-t.at)/1000)
 if(seconds<15 and t.xp==0 and t.damage<=p:getMaxHealth()*.35)or reason=='PROFILE_CHANGED'then
  self.brain.emit('PROGRESSION_AREA_CENSORED',{region=t.region,reason=reason,seconds=seconds});self.trial=nil;self.trialGoal=nil;return
 end
 local bad=t.xp<=0 or t.damage>p:getMaxHealth()*.35
 local id=t.region..'|'..t.profile;local r=self.records[id]or{n=0,xp=0,damage=0,seconds=0,bad=0};self.records[id]=r
 r.n=r.n+1;r.xp=r.xp+t.xp;r.damage=r.damage+t.damage;r.seconds=r.seconds+seconds;r.bad=r.bad+(bad and 1 or 0)
 if bad then self.cooldown[t.region]=clock()+120000 end
 self.brain.emit('PROGRESSION_AREA_ASSESSED',{region=t.region,profile=t.profile,xp=t.xp,damage=t.damage,seconds=seconds,result=bad and'RECONSIDER'or'KEEP_CANDIDATE',reason=reason,causalAttribution='OBSERVED_DURING_TRIAL'})
 self.trial=nil;self.trialGoal=nil
end
function P:observe(p,gain,lost)
 self:leaseQuest(self.brain.goal)
 if self.trial then
  self.trial.xp=self.trial.xp+math.max(0,gain);self.trial.damage=self.trial.damage+math.max(0,lost)
  if self:profile(p).key~=self.trial.profile then self:evaluate(p,'PROFILE_CHANGED')
  elseif clock()-self.trial.at>=60000 or p:getHealth()/p:getMaxHealth()<.6 or region(p:getPosition())~=self.trial.region then self:evaluate(p,'WINDOW_OR_HEALTH_OR_AREA_CHANGED')end
 elseif self.trialGoal then
  local q=p:getPosition();local g=self.trialGoal
  if q.z==g.pos.z and math.max(math.abs(q.x-g.pos.x),math.abs(q.y-g.pos.y))<=8 then self.trial={at=clock(),region=region(q),profile=self:profile(p).key,xp=0,damage=0}
  elseif clock()-g.selectedAt>150000 then self.trialGoal=nil end
 end
 if clock()-self.lastStatusAt<1000 then return end;self.lastStatusAt=clock()
 local needs,name=self:needs(p);local phase='SUSTAIN';local upgrade=self.brain.inventory:upgrade(p)
 if self.kills==0 or self.brain.inventory:foodCount(p)==0 then phase='SUSTAIN'elseif upgrade then phase='IMPROVE'elseif #needs>0 then phase='ACQUIRE'elseif self.trial then phase='ASSESS'elseif self:ready(p)then phase='TRY_AREA'end
 local signature=phase..':'..name..':'..p:getMoney()..':'..self:profile(p).key..':'..#needs
 if signature~=self.statusSignature then
  self.statusSignature=signature;self.brain.emit('PROGRESSION_STATUS',{phase=phase,referenceSet=name,missingReferences=needs,gold=p:getMoney(),reservePreference=self.brain.cfg.progressionReserveGold or 20,equipment=self:profile(p).equipped,confirmedKills=self.kills,confirmedImprovements=self.improvements,canContinueHunting=true})
 end
end
function P:onResult(p,g,status,reason)
 if g and self:ready(p)and(g.kind=='SECTOR'or g.kind=='CIRCUIT'or g.kind=='ENTER')then
  local rat=g.circuitName and g.circuitName:match('^rat')or(g.id or''):match('^SECTOR:rat')or(g.id or''):match('^SECTOR:cave rat')
  if not rat and status~='COMPLETED'and reason~='RAT_FINISH_PRIORITY'and reason~='FINISH_ENGAGED_TARGET'then
   self.advancedFailures=(self.advancedFailures or 0)+1
   if self.advancedFailures>=3 then self.fallbackUntil=clock()+60000;self.advancedFailures=0;self.brain.emit('RAT_FALLBACK_WINDOW',{seconds=60,reason='ADVANCED_ROUTES_FAILED'})end
  end
 end
 if g and g.kind=='HUNT'and status=='COMPLETED'and not(g.id or''):find('rat',1,true)then self.advancedFailures=0;self.fallbackUntil=nil end
 if g and g.progressionQuest then
  local key=g.uid..':'..(self.brain.a.variant or'unknown');if CromeQuestLeases[key]then CromeQuestLeases[key][self.brain.a.name]=nil end
  if status~='COMPLETED'and reason~='RAT_FINISH_PRIORITY'and reason~='FINISH_ENGAGED_TARGET'and reason~='SAFETY_RECONSIDERATION'then
   self.questFailures[g.uid]={untilAt=clock()+300000,links=self:linkCount()}
   self.brain.emit('QUEST_ATTEMPT_DEFERRED',{uid=g.uid,seconds=300,reason=reason,resumeOnNewAccessEvidence=true})
  end
 end
 if status=='COMPLETED'and g and g.kind=='HUNT'and reason=='XP_OBSERVED'then self.kills=self.kills+1 end
 if status=='COMPLETED'and reason=='EQUIPMENT_OBSERVED'then self.improvements=self.improvements+1;self.lastStatusAt=0 end
 if self.trialGoal and g and self.trialGoal.goalId==g.id and status~='COMPLETED'and not self.trial then self.trialGoal=nil end
end
function P:save()
 local rows={'CROME_PROGRESSION_1\t'..self.brain.cfg.domain,'C\tkills\t'..self.kills,'C\timprovements\t'..self.improvements};local keys={}
 for k in pairs(self.records)do keys[#keys+1]=k end;table.sort(keys)
 for _,k in ipairs(keys)do local r=self.records[k];rows[#rows+1]=table.concat({'R',k,r.n,r.xp,r.damage,r.seconds,r.bad},'\t')end
 local quests={};for uid in pairs(self.questFailures)do quests[#quests+1]=uid end;table.sort(quests)
 for _,uid in ipairs(quests)do local q=self.questFailures[uid];rows[#rows+1]=table.concat({'Q',uid,q.untilAt,q.links},'\t')end
 return self.brain.router.nav.atomic(self.brain.stem..'_progression.tsv',table.concat(rows,'\n')..'\n')
end
return P
