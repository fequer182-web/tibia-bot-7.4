CromeAccessLeases=CromeAccessLeases or{}
CromeAccessCooldown=CromeAccessCooldown or{}
local Nav=dofile('data/crome_race/baseline/navigation.lua')
local Transit=dofile('data/crome_race/transit.lua')
local R={};R.__index=R
local function cp(p)return{x=p.x,y=p.y,z=p.z}end
local function key(p)return Nav.key(p)end
local function dist(a,b)if a.z~=b.z then return 999 end;return math.abs(a.x-b.x)+math.abs(a.y-b.y)end
local function visible(p,q)return p.z==q.z and math.abs(p.x-q.x)<=7 and math.abs(p.y-q.y)<=5 end
local dirs={{0,-1,0},{1,0,1},{0,1,2},{-1,0,3}}
function R.new(a,wiki,prior,cfg,inventory,emit)
 local self=setmetatable({a=a,wiki=wiki,cfg=cfg,inventory=inventory,emit=emit,path=nil,pathGoal=nil,entry={},links={},lastPlan=0,failures=0,transitions=0},R)
 self.transitRejected={}
 self.nav=Nav.new{domain=cfg.domain,diagonal=true,path=cfg.memoryRoot..'/'..a.name:gsub('[^%w_]','_')..'_nav.tsv',prior=prior}
 for _,door in ipairs(Transit.data.doors)do self.nav.prior[key(door)]=true end
 self.entryPath=cfg.memoryRoot..'/'..a.name:gsub('[^%w_]','_')..'_access.tsv'
 local f=io.open(self.entryPath,'r')
 if f then
  assert(f:read('*l')=='CROME_ACCESS_1\t'..cfg.domain,'Access memory domain mismatch')
  for line in f:lines()do
   local z,ax,ay,az,px,py,pz,ox,oy,oz=line:match('^(%d+)\t(%d+)\t(%d+)\t(%d+)\t(%d+)\t(%d+)\t(%d+)\t(%d+)\t(%d+)\t(%d+)$')
   assert(z,'Invalid access memory row');z=tonumber(z);assert(z<16 and tonumber(az)==z and tonumber(pz)<16 and tonumber(oz)<16,'Invalid access floor')
   self.entry[z]={arrival={x=tonumber(ax),y=tonumber(ay),z=tonumber(az)},parent={x=tonumber(px),y=tonumber(py),z=tonumber(pz)},access={x=tonumber(ox),y=tonumber(oy),z=tonumber(oz)}}
  end;f:close()
 end
 self.linkPath=cfg.memoryRoot..'/'..a.name:gsub('[^%w_]','_')..'_links.tsv'
 local lf=io.open(self.linkPath,'r')
 if lf then
  assert(lf:read('*l')=='CROME_LINKS_1\t'..cfg.domain,'Link memory domain mismatch')
  for line in lf:lines()do local x,y,z,ax,ay,az=line:match('^(%d+)\t(%d+)\t(%d+)\t(%d+)\t(%d+)\t(%d+)$');assert(x and tonumber(z)<16 and tonumber(az)<16,'Invalid link row');self.links[x..','..y..','..z]={x=tonumber(ax),y=tonumber(ay),z=tonumber(az)}end
  lf:close()
 end
 self.nav:visit(a.spawn)
 return self
end
function R:find(p,q,predicate)
 if not visible(p:getPosition(),q)then return end
 local t=Tile(Position(q.x,q.y,q.z));if not t then return end
 local items=t:getItems()or{};local g=t:getGround();if g then items[#items+1]=g end
 for _,v in ipairs(items)do if predicate(v)then return v end end
end
function R:door(p,q)
 return self:find(p,q,function(item)return item:getType():isDoor()end)
end
function R:keyFor(p,door)
 for _,row in ipairs(self.inventory:scan(p))do if row.id>=2087 and row.id<=2092 and row.ref:getActionId()==door:getActionId()then return row.ref end end
end
function R:blockedByCreature(p,tile)
 for _,c in ipairs(tile:getCreatures()or{})do
  if c~=p then
   local peer=c:isPlayer()and c.isCromeInternalBot and c:isCromeInternalBot()
   local internal=p.isCromeInternalBot and p:isCromeInternalBot()
   if not(peer and internal)then return true end
  end
 end
 return false
end
function R:observe(p)
 local here=p:getPosition()
 for dx=-7,7 do for dy=-5,5 do
  local q={x=here.x+dx,y=here.y+dy,z=here.z};local t=Tile(Position(q.x,q.y,q.z))
  if t and self:blockedByCreature(p,t)then self.nav:observe(q,false,'CREATURE')
  elseif t and t:getFieldItem()then self.nav:observe(q,false,'HAZARD')
  else
   local door=self:door(p,q);local can=p:cromeCanStep(Position(q.x,q.y,q.z));if t and t:getTopCreature()==p then can=true end
   if door and(not table.contains(lockedDoors,door:getId())or self:keyFor(p,door))then can=true end
   self.nav:observe(q,can,'STATIC')
  end
 end end
end
function R:poll(p)
 local j=self.pending
 if j then
  local here=p:getPosition()
  if j.kind=='ACCESS'then
   if here.z~=j.from.z then
    local k=key(j.access.pos);local lease=CromeAccessLeases[k];if lease and lease.owner==self.a.name then CromeAccessLeases[k]=nil end
    self.pending=nil;self.floorExit=nil;self.path=nil;self.pathGoal=nil;self.transitions=self.transitions+1
    local expected=j.from.z+j.access.delta
    if here.z~=expected or math.max(math.abs(here.x-j.access.pos.x),math.abs(here.y-j.access.pos.y))>4 then self.emit('ACCESS_UNEXPECTED','from='..key(j.from)..' to='..key(here));return{status='UNCERTAIN',reason='UNEXPECTED_FLOOR'}end
    self.links[key(j.access.pos)]=cp(here)
    if here.z~=7 and(not self.entry[here.z]or math.abs(here.z-7)>math.abs(j.from.z-7))then self.entry[here.z]={arrival=cp(here),parent=cp(j.from),access=cp(j.access.pos)}end
    if here.z==7 then self.entry[j.from.z]=nil end
    self.emit('ACCESS_COMPLETED','id='..j.access.id..' from='..key(j.from)..' object='..key(j.access.pos)..' to='..key(here))
    return{status='COMPLETED',kind='ACCESS',novel=self.nav:visit(here)}
   end
  elseif j.kind=='DOOR'then
   local item=self:door(p,j.pos)
   if item and item:getId()~=j.id then self.pending=nil;self.emit('DOOR_COMPLETED','pos='..key(j.pos)..' id='..item:getId());return{status='COMPLETED',kind='DOOR'}end
  end
  if os.mtime()-j.at>3500 then if j.kind=='ACCESS'then local k=key(j.access.pos);CromeAccessLeases[k]=nil;CromeAccessCooldown[k]=os.mtime()+3000 end;self.pending=nil;self.path=nil;self.failures=self.failures+1;self.emit('INTERACTION_UNCERTAIN','kind='..j.kind);return{status='UNCERTAIN',reason=j.kind..'_TIMEOUT'}end
  return{status='SENT'}
 end
 local result=self.nav:observePosition(p:getPosition())
 if result and result.status=='UNCERTAIN'then self.path=nil;self.failures=self.failures+1;self.emit('MOVE_UNCERTAIN','reason='..result.reason)
 elseif result and result.status=='COMPLETED'then self.emit('MOVE_COMPLETED','pos='..key(p:getPosition())..' novel='..tostring(result.novel))end
 return result
end
function R:advanceSearch(p)
 local work=self.searching;if not work then return 'IDLE'end
 local here=p:getPosition()
 if key(here)~=key(work.from)or self.pathGoal~=work.key then self.searching=nil;return 'CANCELLED'end
 if os.mtime()-work.at>30000 then self.searching=nil;self.searchOutcome='SEARCH_TIMEOUT';return 'SEARCH_TIMEOUT'end
 local priorBudget=self.nav.searchBudgetMs;local priorSpent=self.nav.searchSpent or 0
 self.nav.searchBudgetMs=2;self.nav.searchSpent=0
 local plan=self.nav:plan(work.from,work.goal,{adjacent=work.adjacent,returning=work.returning,limit=work.limit,radius=180,retain=true})
 local cost=self.nav.searchSpent or 0;self.nav.searchBudgetMs=priorBudget;self.nav.searchSpent=priorSpent+cost
 work.expanded=plan.expanded
 if plan.status=='DEFERRED'then return 'PENDING'end
 if plan.status=='BUDGET_EXHAUSTED'and work.limit<12000 then work.limit=math.min(12000,work.limit*2);return 'PENDING'end
 self.searching=nil
 if plan.status=='ROUTE'then
  self.path=plan.path;self.searchWaits=nil
  self.emit('ROUTE_SEARCH_RESULT',{goal=key(work.goal),status='ROUTE',expanded=plan.expanded,millis=os.mtime()-work.at})
  return 'ROUTE'
 end
 self.searchOutcome=plan.status=='BUDGET_EXHAUSTED'and'SEARCH_LIMIT'or plan.status
 self.emit('ROUTE_SEARCH_RESULT',{goal=key(work.goal),status=self.searchOutcome,expanded=plan.expanded,millis=os.mtime()-work.at})
 return self.searchOutcome
end

function R:move(p,goal,adjacent,returning)
 if self.pending or self.nav.pending then return 'PENDING' end
 local here=p:getPosition();if here.z~=goal.z then return 'OTHER_FLOOR'end
 if(adjacent and dist(here,goal)<=1)or(not adjacent and key(here)==key(goal))then return 'ARRIVED'end
 self.fastRequest={goal=cp(goal),adjacent=adjacent,returning=returning}
 local gk=key(goal)..':'..tostring(adjacent)
 if self.pathGoal~=gk then self.path=nil;self.pathGoal=gk;self.searching=nil;self.searchOutcome=nil end
 if self.searchOutcome then return self.searchOutcome end
 if self.searching then return 'WAIT'end
 if self.path then while self.path[1]and key(self.path[1].q)==key(here)do table.remove(self.path,1)end end
 if not self.path or not self.path[1]or math.max(math.abs(here.x-self.path[1].q.x),math.abs(here.y-self.path[1].q.y))~=1 then
  if os.mtime()-self.lastPlan<500 then return 'WAIT'end;self.lastPlan=os.mtime()
  self.searching={from=cp(here),goal=cp(goal),key=gk,adjacent=adjacent,returning=returning,limit=3500,at=os.mtime(),expanded=0}
  local status=self:advanceSearch(p)
  if status~='ROUTE'then return status=='PENDING'and'WAIT'or status end
 end
 local step=self.path[1];if not step then return 'NO_ROUTE'end
 if self.avoid and self.avoid(step.q)then self.path=nil;return 'KNOWN_DANGER'end
 if not self.nav:walkable(step.q)or not p:cromeCanStep(Position(step.q.x,step.q.y,step.q.z))then
  local door=self:door(p,step.q)
  if door and dist(here,step.q)<=1 then
   local id=door:getId();local keyItem=table.contains(lockedDoors,id)and self:keyFor(p,door)or nil
   if table.contains(lockedDoors,id)and not keyItem then self.path=nil;self.emit('KEY_REQUIRED','pos='..key(step.q)..' aid='..door:getActionId());return 'KEY_REQUIRED'end
   local accepted=keyItem and p:cromeUseObject(keyItem,door)or p:cromeUseObject(door)
   if accepted then self.pending={kind='DOOR',id=id,pos=cp(step.q),at=os.mtime()}end
   return 'PENDING'
  end
  self.path=nil;return 'BLOCKED'
 end
 if p:cromeStep(step.d)then self.nav:sent(cp(here),step.q);return 'SENT'end
 return 'WAIT'
end
function R:accessAt(p,ref)
 return self:find(p,ref.pos,function(item)return item:getId()==ref.id end)
end
function R:enter(p,ref)
 if self.pending or self.nav.pending then return 'PENDING'end
 local here=p:getPosition();local leaseKey=key(ref.pos);local lease=CromeAccessLeases[leaseKey]
 if(CromeAccessCooldown[leaseKey]or 0)>os.mtime()then self.path=nil;self.fastRequest=nil;return 'ACCESS_BUSY'end
 if dist(here,ref.pos)<=4 then
  if lease and lease.owner~=self.a.name and lease.untilAt>os.mtime()then
   self.path=nil;self.fastRequest=nil
   if dist(here,ref.pos)<=2 then
    for _,d in ipairs(dirs)do local q={x=here.x+d[1],y=here.y+d[2],z=here.z}
     if dist(q,ref.pos)>2 and self.nav:walkable(q)and(not self.avoid or not self.avoid(q))and p:cromeCanStep(Position(q.x,q.y,q.z))and p:cromeStep(d[3])then self.nav:sent(cp(here),q);break end
    end
   end
   return 'ACCESS_BUSY'
  end
  CromeAccessLeases[leaseKey]={owner=self.a.name,untilAt=os.mtime()+5000}
 end
 local status=self:move(p,ref.pos,true)
 if status~='ARRIVED'then if status=='NO_ROUTE'or status=='BLOCKED'then self.floorExit=nil end;return status end
 local item=self:accessAt(p,ref)
 if not item then self.emit('ACCESS_REFERENCE_CHANGED','pos='..key(ref.pos)..' id='..ref.id);return 'REFERENCE_CHANGED'end
 local accepted=false
 if ref.method=='use'then accepted=p:cromeUseObject(item)
 else
  for _,d in ipairs(dirs)do if here.x+d[1]==ref.pos.x and here.y+d[2]==ref.pos.y then accepted=p:cromeStep(d[3],true);break end end
 end
 if accepted then self.pending={kind='ACCESS',from=cp(here),access=ref,at=os.mtime()};self.emit('ACCESS_SENT','id='..ref.id..' pos='..key(ref.pos))end
 return 'PENDING'
end
function R:betweenLegacy(p,goal)
 local here=p:getPosition();local direction=goal.z>here.z and 1 or-1
 local targetKey=key(goal)
 if self.floorExit and self.floorExit.from==here.z and self.floorExit.goal==targetKey then
  return self:enter(p,self.floorExit.ref)
 end
 if os.mtime()-(self.lastExitPlan or -1000)<500 then return 'WAIT'end
 self.lastExitPlan=os.mtime()
 local parent=self.entry[here.z];local anchor=parent and parent.arrival or here;local choices={}
 for _,ref in ipairs(self.wiki.accesses)do
  if ref.pos.z==here.z and ref.delta==direction then
   local score=dist(ref.pos,anchor)*2+dist(here,ref.pos)*.1
   if goal.z~=7 then score=dist(here,ref.pos)+math.abs(ref.pos.x-goal.x)+math.abs(ref.pos.y-goal.y)end
   choices[#choices+1]={ref=ref,score=score}
  end
 end
 table.sort(choices,function(a,b)return a.score<b.score end)
 local first=self.exitSearchOffset or 1
 local attempted=0
 for offset=0,math.min(3,#choices-1)do
  local index=(first+offset-1)%#choices+1;local choice=choices[index];attempted=attempted+1
  local approachKey=key(here)..'>'..key(choice.ref.pos)
  local cached=self.exitApproach
  local plan
  if cached and cached.key==approachKey and cached.untilAt>os.mtime()then plan=cached.plan
  else plan=self.nav:plan(here,choice.ref.pos,{adjacent=true,returning=true,limit=3500,radius=180});if plan.status=='ROUTE'or plan.status=='REACHED'then self.exitApproach={key=approachKey,plan=plan,untilAt=os.mtime()+15000}end end
  if plan.status=='DEFERRED'then self.exitSearchOffset=index;return 'WAIT'end
  local destination=self.links[key(choice.ref.pos)];local usable=true
  if destination and destination.z==goal.z then
   local onward=self.nav:plan(destination,goal,{adjacent=true,returning=true,limit=3500,radius=180})
   if onward.status=='DEFERRED'then self.exitSearchOffset=index;return 'WAIT'end
   usable=onward.status=='ROUTE'or onward.status=='REACHED'
  end
  if usable and(plan.status=='ROUTE'or plan.status=='REACHED')then
   self.floorExit={from=here.z,goal=targetKey,ref=choice.ref};if plan.path then self.path=plan.path;self.pathGoal=key(choice.ref.pos)..':true'end;return self:enter(p,choice.ref)
  end
 end
 self.exitSearchOffset=#choices>0 and((first+attempted-1)%#choices+1)or 1
 return #choices==0 and 'NO_ACCESS'or'NO_ROUTE'
end
function R:between(p,goal)
 local edge,status=Transit.next(p:getPosition(),goal,self.transitRejected or{})
 if edge then
  local outcome=self:enter(p,edge.access)
  if outcome=='NO_ROUTE'or outcome=='BLOCKED'or outcome=='KEY_REQUIRED'or outcome=='REFERENCE_CHANGED'or outcome=='SEARCH_LIMIT'or outcome=='SEARCH_TIMEOUT'then
   self.transitRejected=self.transitRejected or{};self.transitRejected[edge.key]=os.mtime()+60000;self.floorExit=nil;self.path=nil;self.pathGoal=nil;self.searching=nil;self.searchOutcome=nil
   self.emit('TRANSIT_EDGE_DEFERRED',{access=key(edge.access.pos),reason=outcome})
  end
  return outcome
 end
 if status=='NO_ROUTE'then return 'NO_ROUTE'end
 return self:betweenLegacy(p,goal)
end
function R:vendorGoal(p,npc)
 local here=p:getPosition();local ck=key(npc)..':'..tostring(Transit.component(here))
 self.vendorCache=self.vendorCache or{};local cached=self.vendorCache[ck]
 if cached and cached.untilAt>os.mtime()then return cp(cached.goal)end
 local best=nil;local score=nil
 for dx=-3,3 do for dy=-3,3 do
  local q={x=npc.x+dx,y=npc.y+dy,z=npc.z}
  if(dx~=0 or dy~=0)and Transit.data.cells[key(q)]then
   local at=Position(q.x,q.y,q.z);local target=Position(npc.x,npc.y,npc.z)
   local visible=not at.isSightClear or at:isSightClear(target,true)
   local hops=Transit.hops(here,q,self.transitRejected or{})
   if visible and hops and hops<math.huge then
    local value=hops*100+math.abs(here.x-q.x)+math.abs(here.y-q.y)+math.max(math.abs(dx),math.abs(dy))*.1
    if not score or value<score then best=q;score=value end
   end
  end
 end end
 if best then self.vendorCache[ck]={goal=best,untilAt=os.mtime()+10000};return cp(best)end
 return cp(npc)
end
function R:go(p,goal,adjacent,returning)
 local here=p:getPosition();local hops=Transit.hops(here,goal,self.transitRejected or{})
 if here.z~=goal.z or(hops and hops>0 and hops<math.huge)then return self:between(p,goal)end
 return self:move(p,goal,adjacent,returning)
end
-- Candidate screening is read-only: no step or interaction is sent here.
function R:assess(p,goal,adjacent)
 local here=p:getPosition()
 local hops=Transit.hops(here,goal,self.transitRejected or{})
 if hops==math.huge then return 'NO_ROUTE'end
 if hops and hops>0 then return 'REFERENCE_ONLY'end
 if here.z~=goal.z then
  local direction=goal.z>here.z and 1 or -1
  for _,ref in ipairs(self.wiki.accesses)do if ref.pos.z==here.z and ref.delta==direction then return 'REFERENCE_ONLY' end end
  return 'NO_ACCESS'
 end
 self.assessCache=self.assessCache or{}
 local cacheKey=math.floor(here.x/4)..','..math.floor(here.y/4)..','..here.z..':'..key(goal)..':'..tostring(adjacent)
 local cached=self.assessCache[cacheKey];if cached and cached.untilAt>os.mtime()then return cached.status end
 if dist(here,goal)>12 then return 'REFERENCE_ONLY'end
 local plan=self.nav:plan(here,goal,{preview=true,adjacent=adjacent,limit=500,radius=24})
 if plan.status=='DEFERRED'or plan.status=='BUDGET_EXHAUSTED'then return 'REFERENCE_ONLY'end
 local status=(plan.status=='ROUTE'or plan.status=='REACHED')and 'REACHABLE'or plan.status
 if self.assessCacheSize and self.assessCacheSize>200 then self.assessCache={};self.assessCacheSize=0 end
 self.assessCache[cacheKey]={status=status,untilAt=os.mtime()+2500};self.assessCacheSize=(self.assessCacheSize or 0)+1
 return status
end

-- Fast path executor: never searches a route or uses an access/door here.
function R:pump(p)
 if self.pending then return nil,'HOLD' end
 local result=self:poll(p)
 if result then
  result.position=cp(p:getPosition())
  if result.status=='SENT'then return nil,'PENDING'end
  if result.status~='COMPLETED'then return result,'NEEDS_THINK'end
 end
 local request=self.fastRequest
 if not request or not self.path then return result,'HOLD'end
 local here=p:getPosition()
 if here.z~=request.goal.z then return result,'NEEDS_THINK'end
 if(request.adjacent and dist(here,request.goal)<=1)or(not request.adjacent and key(here)==key(request.goal))then return result,'NEEDS_THINK'end
 while self.path[1]and key(self.path[1].q)==key(here)do table.remove(self.path,1)end
 local step=self.path[1]
 if not step or math.max(math.abs(here.x-step.q.x),math.abs(here.y-step.q.y))~=1 then return result,'NEEDS_THINK'end
 if not self.nav:walkable(step.q)or not p:cromeCanStep(Position(step.q.x,step.q.y,step.q.z))then return result,'NEEDS_THINK'end
 if p:cromeStep(step.d)then self.nav:sent(cp(here),step.q)end
 return result,'CONTINUE'
end

function R:save()
 local ok,err=self.nav:save();if not ok then return ok,err end
 local rows={'CROME_ACCESS_1\t'..self.cfg.domain}
 for z=0,15 do local e=self.entry[z];if e then rows[#rows+1]=table.concat({z,e.arrival.x,e.arrival.y,e.arrival.z,e.parent.x,e.parent.y,e.parent.z,e.access.x,e.access.y,e.access.z},'\t')end end
 ok,err=Nav.atomic(self.entryPath,table.concat(rows,'\n')..'\n');if not ok then return ok,err end
 local links={'CROME_LINKS_1\t'..self.cfg.domain};local keys={};for k in pairs(self.links)do keys[#keys+1]=k end;table.sort(keys)
 for _,k in ipairs(keys)do local v=self.links[k];links[#links+1]=k:gsub(',','\t')..'\t'..v.x..'\t'..v.y..'\t'..v.z end
 return Nav.atomic(self.linkPath,table.concat(links,'\n')..'\n')
end
R.key=key;R.distance=dist;R.copy=cp
return R
