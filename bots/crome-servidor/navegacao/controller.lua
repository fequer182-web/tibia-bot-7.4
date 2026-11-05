local Nav=dofile('data/crome_learning/navigation.lua')
local Policy=dofile('data/crome_learning/policy.lua')
local C={};C.__index=C
local function key(p)return Nav.key(p)end
local function copy(p)return{x=p.x,y=p.y,z=p.z}end
function C.new(p,cfg,prior,emit,stop)
 local stem=cfg.memoryRoot..'/'..p:getName():gsub('[^%w_]','_')
 local c=setmetatable({anchor=copy(p:getPosition()),emit=emit,stop=stop,started=os.mtime(),lastHp=p:getHealth(),macro=nil,forced=false,choices=0,goalRadius=cfg.goalRadius or 18},C)
 c.nav=Nav.new{domain=cfg.domain,path=stem..'_nav.tsv',prior=prior}
 c.policy=Policy.new{domain=cfg.domain,path=stem..'_policy.tsv',atomic=Nav.atomic,seed=p:getGuid()+17}
 c.emit('LEARNING_LOADED','tiles='..c.nav.loaded..' updates='..c.policy.updates)
 c.nav:visit(c.anchor)
 return c
end
function C:state(p)
 local here=p:getPosition();local armor=0;local attack=0
 for _,slot in ipairs({1,4,5,6,7,8})do local item=p:getSlotItem(slot);if item then local t=item:getType();armor=armor+t:getArmor();attack=math.max(attack,t:getAttack())end end
 return table.concat({'v1',here.z,math.floor(here.x/16),math.floor(here.y/16),math.min(4,math.floor(p:getHealth()/p:getMaxHealth()*5)),math.floor(armor/5),math.floor(attack/5)},':')
end
function C:observe(p)
 local here=p:getPosition()
 for dx=-7,7 do for dy=-5,5 do
  local q={x=here.x+dx,y=here.y+dy,z=here.z};local tile=Tile(Position(q.x,q.y,q.z))
  local reason=tile and tile:getTopCreature()and'CREATURE'or(tile and tile:getFieldItem()and'HAZARD'or'STATIC')
  self.nav:observe(q,p:cromeCanStep(Position(q.x,q.y,q.z)),reason)
 end end
end
function C:candidates(p)
 local list=self.nav:frontiers(p:getPosition(),self.anchor,self.goalRadius);local valid={}
 for _,a in ipairs(list)do local plan=self.nav:plan(p:getPosition(),a.goal,{limit=1500});if plan.status=='ROUTE'then valid[#valid+1]=a end end
 if #valid==0 then valid[1]={id='WAIT',kind='WAIT',goal=copy(p:getPosition()),score=-10}end
 return valid
end
function C:finish(p,status,terminal)
 if not self.policy.pending then return end
 local r=self.policy:finish(status,self:state(p),terminal and{}or self:candidates(p),terminal)
 self.emit('POLICY_UPDATED','state='..r.state..' status='..r.status..' action='..r.action..' reward='..string.format('%.4f',r.reward)..' old='..string.format('%.4f',r.old)..' q='..string.format('%.4f',r.value)..' confirmed='..r.confirmed..' updates='..r.updates)
 self.macro=nil
 local ok,why=self.nav:save();assert(ok,tostring(why));ok,why=self.policy:save();assert(ok,tostring(why))
end
function C:tick(p)
 self:observe(p)
 local hp=p:getHealth();local lost=math.max(0,self.lastHp-hp);self.lastHp=hp
 if lost>0 and self.policy.pending then self.policy:reward('HP_LOSS',-.08*lost)end
 local observed=self.nav:observePosition(p:getPosition())
 if observed then
  if observed.status=='SENT'then return end
  if observed.status=='COMPLETED'then
   if self.policy.pending then self.policy:confirmed(observed.novel)end
   self.emit('NAV_STEP_COMPLETED','seq='..observed.id..' to='..key(p:getPosition())..' novel='..tostring(observed.novel))
  else
   self.emit('NAV_STEP_UNCERTAIN','reason='..observed.reason)
   self:finish(p,'UNCERTAIN',false)
   if observed.reason=='UNEXPECTED_DISPLACEMENT'then self.stop('LEARNING_UNEXPECTED_DISPLACEMENT');return end
  end
 end
 if hp/p:getMaxHealth()<.6 then
  if not self.forced then self:finish(p,'UNCERTAIN',false);self.emit('SAFETY_RETURN','hp='..hp);self.forced=true end
 end
 if self.forced then
  if key(p:getPosition())==key(self.anchor)then self.stop('LEARNING_SAFE_RETURN');return end
  local plan=self.nav:plan(p:getPosition(),self.anchor,{returning=true})
  if plan.status=='ROUTE'then
   local from=copy(p:getPosition());if p:cromeStep(plan.first.d)then self.nav:sent(from,plan.first.q)end
  end
  return
 end
 if self.macro then
  local age=os.mtime()-self.macro.at
  local reached=key(p:getPosition())==key(self.macro.action.goal)
  if self.macro.action.kind=='WAIT'then
   if age>=1000 then self:finish(p,'UNCERTAIN',false)else return end
  elseif reached then self:finish(p,'COMPLETED',false)
  elseif age>=15000 then self:finish(p,'UNCERTAIN',false)end
 end
 if not self.macro then
  local a,random,q=self.policy:choose(self:state(p),self:candidates(p));self.macro={action=a,at=os.mtime()};self.choices=self.choices+1
  self.emit('POLICY_CHOICE','goal='..a.id..' exploratory='..tostring(random)..' q='..q)
 end
 local a=self.macro.action;if a.kind=='WAIT'then return end
 local plan=self.nav:plan(p:getPosition(),a.goal,{limit=2000})
 if plan.status=='ROUTE'then
  local from=copy(p:getPosition())
  if p:cromeStep(plan.first.d)then self.nav:sent(from,plan.first.q)end
 elseif plan.status=='BUDGET_EXHAUSTED'then self.emit('PLAN_BUDGET_EXHAUSTED','expanded='..plan.expanded)
 end
end
function C:close(p)
 self:finish(p,'UNCERTAIN',true)
 local ok,why=self.nav:save();assert(ok,tostring(why));ok,why=self.policy:save();assert(ok,tostring(why))
 local unique=0;for _,n in pairs(self.nav.visited)do if n>0 then unique=unique+1 end end
 self.emit('LEARNING_SUMMARY','confirmed='..self.nav.confirmed..' uncertain='..self.nav.uncertain..' uniqueLifetime='..unique..' loadedTiles='..self.nav.loaded..' updates='..self.policy.updates..' choices='..self.choices)
end
return C
