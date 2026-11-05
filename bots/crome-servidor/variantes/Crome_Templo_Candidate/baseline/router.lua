local Nav=dofile('data/crome_race/baseline/navigation.lua')
local R={};R.__index=R
local function cp(p)return{x=p.x,y=p.y,z=p.z}end
local function key(p)return Nav.key(p)end
local function dist(a,b)if a.z~=b.z then return 999 end;return math.abs(a.x-b.x)+math.abs(a.y-b.y)end
local function visible(p,q)return p.z==q.z and math.abs(p.x-q.x)<=7 and math.abs(p.y-q.y)<=5 end
local dirs={{0,-1,0},{1,0,1},{0,1,2},{-1,0,3}}
function R.new(a,wiki,prior,cfg,inventory,emit)
 local self=setmetatable({a=a,wiki=wiki,cfg=cfg,inventory=inventory,emit=emit,path=nil,pathGoal=nil,entry={},links={},lastPlan=0,failures=0,transitions=0},R)
 self.nav=Nav.new{domain=cfg.domain,diagonal=true,path=cfg.memoryRoot..'/'..a.name:gsub('[^%w_]','_')..'_nav.tsv',prior=prior}
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
function R:observe(p)
 local here=p:getPosition()
 for dx=-7,7 do for dy=-5,5 do
  local q={x=here.x+dx,y=here.y+dy,z=here.z};local t=Tile(Position(q.x,q.y,q.z))
  if t and t:getTopCreature()and t:getTopCreature()~=p then self.nav:observe(q,false,'CREATURE')
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
    self.pending=nil;self.floorExit=nil;self.path=nil;self.pathGoal=nil;self.transitions=self.transitions+1
    local expected=j.from.z+j.access.delta
    if here.z~=expected or math.max(math.abs(here.x-j.access.pos.x),math.abs(here.y-j.access.pos.y))>4 then self.emit('ACCESS_UNEXPECTED','from='..key(j.from)..' to='..key(here));return{status='UNCERTAIN',reason='UNEXPECTED_FLOOR'}end
    self.links[key(j.access.pos)]=cp(here)
    if here.z~=7 and not self.entry[here.z]then self.entry[here.z]={arrival=cp(here),parent=cp(j.from),access=cp(j.access.pos)}end
    if here.z==7 then self.entry[j.from.z]=nil end
    self.emit('ACCESS_COMPLETED','id='..j.access.id..' from='..key(j.from)..' object='..key(j.access.pos)..' to='..key(here))
    return{status='COMPLETED',kind='ACCESS',novel=self.nav:visit(here)}
   end
  elseif j.kind=='DOOR'then
   local item=self:door(p,j.pos)
   if item and item:getId()~=j.id then self.pending=nil;self.emit('DOOR_COMPLETED','pos='..key(j.pos)..' id='..item:getId());return{status='COMPLETED',kind='DOOR'}end
  end
  if os.mtime()-j.at>3500 then self.pending=nil;self.path=nil;self.failures=self.failures+1;self.emit('INTERACTION_UNCERTAIN','kind='..j.kind);return{status='UNCERTAIN',reason=j.kind..'_TIMEOUT'}end
  return{status='SENT'}
 end
 local result=self.nav:observePosition(p:getPosition())
 if result and result.status=='UNCERTAIN'then self.path=nil;self.failures=self.failures+1;self.emit('MOVE_UNCERTAIN','reason='..result.reason)
 elseif result and result.status=='COMPLETED'then self.emit('MOVE_COMPLETED','pos='..key(p:getPosition())..' novel='..tostring(result.novel))end
 return result
end
function R:move(p,goal,adjacent,returning)
 if self.pending or self.nav.pending then return 'PENDING' end
 local here=p:getPosition();if here.z~=goal.z then return 'OTHER_FLOOR'end
 if(adjacent and dist(here,goal)<=1)or(not adjacent and key(here)==key(goal))then return 'ARRIVED'end
 local gk=key(goal)..':'..tostring(adjacent)
 if self.pathGoal~=gk then self.path=nil;self.pathGoal=gk end
 if self.path then while self.path[1]and key(self.path[1].q)==key(here)do table.remove(self.path,1)end end
 if not self.path or not self.path[1]or math.max(math.abs(here.x-self.path[1].q.x),math.abs(here.y-self.path[1].q.y))~=1 then
  if os.mtime()-self.lastPlan<500 then return 'WAIT'end;self.lastPlan=os.mtime()
  local plan=self.nav:plan(here,goal,{adjacent=adjacent,returning=returning,limit=3500,radius=180})
  if plan.status~='ROUTE'then return plan.status end
  self.path=plan.path;self.pathGoal=gk
 end
 local step=self.path[1];if not step then return 'NO_ROUTE'end
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
 local here=p:getPosition();local status=self:move(p,ref.pos,true)
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
function R:between(p,goal)
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
 for _,choice in ipairs(choices)do
  local plan=self.nav:plan(here,choice.ref.pos,{adjacent=true,returning=true,limit=3500,radius=180})
  local destination=self.links[key(choice.ref.pos)];local usable=true
  if destination and destination.z==goal.z then
   local onward=self.nav:plan(destination,goal,{adjacent=true,returning=true,limit=3500,radius=180})
   usable=onward.status=='ROUTE'or onward.status=='REACHED'
  end
  if usable and(plan.status=='ROUTE'or plan.status=='REACHED')then
   self.floorExit={from=here.z,goal=targetKey,ref=choice.ref};return self:enter(p,choice.ref)
  end
 end
 return #choices==0 and 'NO_ACCESS'or'NO_ROUTE'
end
function R:go(p,goal,adjacent,returning)
 if p:getPosition().z~=goal.z then return self:between(p,goal)end
 return self:move(p,goal,adjacent,returning)
end
-- Candidate screening is read-only: no step or interaction is sent here.
function R:assess(p,goal,adjacent)
 local here=p:getPosition()
 if here.z~=goal.z then
  local direction=goal.z>here.z and 1 or -1
  for _,ref in ipairs(self.wiki.accesses)do if ref.pos.z==here.z and ref.delta==direction then return 'REFERENCE_ONLY' end end
  return 'NO_ACCESS'
 end
 self.assessCache=self.assessCache or{}
 local cacheKey=math.floor(here.x/4)..','..math.floor(here.y/4)..','..here.z..':'..key(goal)..':'..tostring(adjacent)
 local cached=self.assessCache[cacheKey];if cached and cached.untilAt>os.mtime()then return cached.status end
 local plan=self.nav:plan(here,goal,{adjacent=adjacent,limit=1800,radius=180})
 local status=(plan.status=='ROUTE'or plan.status=='REACHED')and 'REACHABLE'or plan.status
 if self.assessCacheSize and self.assessCacheSize>200 then self.assessCache={};self.assessCacheSize=0 end
 self.assessCache[cacheKey]={status=status,untilAt=os.mtime()+2500};self.assessCacheSize=(self.assessCacheSize or 0)+1
 return status
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
