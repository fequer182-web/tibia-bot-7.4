local I={}
function I.new(wiki)
 local o={wiki=wiki,corpses={},buyers={}}
 for _,m in pairs(wiki.monsters)do local id=m.corpse;local seen={};while id and id>0 and not seen[id]do seen[id]=true;o.corpses[id]=true;id=(wiki.items[id]or{}).decayTo end end
 for _,v in ipairs(wiki.vendors)do for _,offer in ipairs(v.sell)do o.buyers[offer.id]=o.buyers[offer.id]or{};o.buyers[offer.id][#o.buyers[offer.id]+1]={vendor=v,offer=offer}end end
 return setmetatable(o,{__index=I})
end
function I:scan(p)
 local result={};local function walk(item,slot,depth)
  if not item or depth>8 then return end
  local id=item:getId();result[#result+1]={ref=item,id=id,count=item:getCount(),slot=slot,body=self.corpses[id]or false,info=self.wiki.items[id]or{}}
  if item:isContainer()then for i=0,item:getSize()-1 do walk(item:getItem(i),nil,depth+1)end end
 end
 for slot=1,10 do walk(p:getSlotItem(slot),slot,0)end
 return result
end
function I:destination(p,id)
 for _,row in ipairs(self:scan(p))do
  local bag=row.ref
  if bag:isContainer()and not row.body then
   if bag:getSize()<bag:getCapacity()then return bag end
   local t=ItemType(id)
   if t:isStackable()then for i=0,bag:getSize()-1 do local v=bag:getItem(i);if v:getId()==id and v:getCount()<100 then return bag end end end
  end
 end
end
function I:foodCount(p)local n=0;for id in pairs(self.wiki.foods)do n=n+p:getItemCount(id)end;return n end
function I:feed(p,a,emit)
 if os.mtime()-(a.lastFood or 0)<2000 then return end
 local c=p:getCondition(CONDITION_REGENERATION,CONDITIONID_DEFAULT)
 if c and c:getTicks()>30000 then return end
 for _,row in ipairs(self:scan(p))do
  if self.wiki.foods[row.id]then
   local before=p:getItemCount(row.id);a.lastFood=os.mtime();p:cromeUseFood(row.ref)
   local eaten=before-p:getItemCount(row.id)
   if eaten>0 then emit('FOOD_CONFIRMED','id='..row.id..' count='..eaten..' seconds='..self.wiki.foods[row.id])end
   return
  end
 end
end
function I:slot(info)
 if info.weapon and info.weapon~=''and info.weapon~='shield'and info.weapon~='distance'and info.weapon~='ammunition'then return 6 end
 if info.weapon=='shield'then return 5 end
 return({head=1,body=4,legs=7,feet=8,necklace=2,ring=9})[info.slot]
end
function I:utility(p,info,slot)
 if not slot then return 0 end
 if slot==6 then local skills={club=1,sword=2,axe=3};return(info.attack or 0)*(p:getSkillLevel(skills[info.weapon]or 0)+10)/20+(info.defense or 0)*.08 end
 return(info.armor or 0)*2+(info.defense or 0)*.12
end
function I:upgrade(p)
 local best=nil
 for _,row in ipairs(self:scan(p))do
  local slot=self:slot(row.info);if slot and row.slot~=slot then
   if slot then local old=p:getSlotItem(slot);local oldInfo=old and self.wiki.items[old:getId()]or{};local gain=self:utility(p,row.info,slot)-self:utility(p,oldInfo,slot)
    if gain>0 and(not best or gain>best.gain)then best={row=row,slot=slot,gain=gain}end
   end
  end
 end
 return best
end
function I:sellOptions(p)
 local options={}
 for _,row in ipairs(self:scan(p))do
  if (not row.slot or row.body)and self.buyers[row.id]and not(self.wiki.foods[row.id]or row.id==2120 or row.id==2554 or(row.id>=2087 and row.id<=2092))then
   local empty=not row.body or row.ref:getSize()==0
   if empty then for _,b in ipairs(self.buyers[row.id])do options[#options+1]={kind='SELL',vendor=b.vendor,item=row.id,name=b.offer.name,price=b.offer.price,ref=row.ref,count=p:getItemCount(row.id)}end end
  end
 end
 return options
end
function I:buyOptions(p)
 local result={};local money=p:getMoney()
 for _,v in ipairs(self.wiki.vendors)do for _,offer in ipairs(v.buy)do
  if offer.price>0 and offer.price<=money then
   local info=self.wiki.items[offer.id]or{};local slot=self:slot(info)
   if slot and info.slot~='two-handed'and info.weapon~='distance'then
    local old=p:getSlotItem(slot);local gain=self:utility(p,info,slot)-self:utility(p,old and self.wiki.items[old:getId()]or{},slot)
    if gain>0 and p:getItemCount(offer.id)==0 then result[#result+1]={kind='BUY',vendor=v,item=offer.id,name=offer.name,price=offer.price,gain=gain,slot=slot}end
   elseif offer.id==1988 and p:getItemCount(1988)<2 and(self:destination(p,1988)or not p:getSlotItem(3))then
    local bag=p:getSlotItem(3);if not bag or bag:getSize()>=bag:getCapacity()-2 then result[#result+1]={kind='BUY',vendor=v,item=offer.id,name=offer.name,price=offer.price,gain=2}end
   end
  end
 end end
 return result
end
function I:discard(p,emit)
 for _,row in ipairs(self:scan(p))do if row.body and not self.buyers[row.id] and(not row.ref:isContainer()or row.ref:getSize()==0)then local id=row.id;if p:cromeMoveOwned(row.ref,0)then emit('DISCARD_DECAYED_CORPSE_CONFIRMED','id='..id);return true end end end
 local haveSpace=self:destination(p,2382)~=nil
 if haveSpace and p:getFreeCapacity()>5000 then return false end
 for _,row in ipairs(self:scan(p))do
  if not row.slot and not(self.buyers[row.id]or self.wiki.foods[row.id]or row.body or row.ref:isContainer()or row.id==2120 or row.id==2554 or(row.id>=2087 and row.id<=2092)or row.id==2148 or row.id==2152 or row.id==2160)then
   local slot=self:slot(row.info)
   if slot then local old=p:getSlotItem(slot);if old and self:utility(p,row.info,slot)<=self:utility(p,self.wiki.items[old:getId()]or{},slot)then
    local id=row.id;local ok=p:cromeMoveOwned(row.ref,0);if ok then emit('DISCARD_OBSOLETE_CONFIRMED','id='..id);return true end
   end end
  end
 end
 return false
end
return I
