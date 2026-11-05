-- Shared short-lived claims; only server-tagged player corpses are eligible.
if CromePlayerScavenge then return CromePlayerScavenge end
local S={claims={}};CromePlayerScavenge=S
local function now()return os.mtime()end
local function key(p)return p.x..','..p.y..','..p.z end
local function walk(b,fn,depth)
 if depth>6 then return end
 for i=0,b:getSize()-1 do local item=b:getItem(i);fn(item);if item:isContainer()then walk(item,fn,depth+1)end end
end
local function count(b,id)local n=0;walk(b,function(i)if i:getId()==id then n=n+i:getCount()end end,0);return n end
function S.release(brain,reason)
 local j=brain.scavengeJob;if not j then return end
 local claim=S.claims[j.key];if claim and claim.owner==brain.a.name then S.claims[j.key]=nil end
 brain.scavengeSkipped=brain.scavengeSkipped or{};brain.scavengeSkipped[j.key]=now()+60000
 brain.emit('PLAYER_LOOT_END',{reason=reason,position=j.pos});brain.scavengeJob=nil
end
local function useful(brain,p,item)
 local id=item:getId();local inv=brain.inventory;local info=brain.wiki.items[id]or{}
 if id==2148 or id==2152 or id==2160 then return 100 end
 if(id==2120 or id==2554)and p:getItemCount(id)==0 then return 50 end
 local slot=inv:slot(info)
 if slot then local worn=p:getSlotItem(slot);local old=worn and brain.wiki.items[worn:getId()]or{}
  local gain=inv:utility(p,info,slot)-inv:utility(p,old,slot)
  if gain>0 and p:getItemCount(id)==0 then return 60+gain end
 end
 if id==1988 and p:getItemCount(id)<2 then return 10 end
 if brain.wiki.foods[id]and not item:getCustomAttribute('crome_lab_food')and inv:foodCount(p)<2 then return 5 end
 return 0
end
function S.tick(brain,p,threats)
 if #threats>0 then S.release(brain,'THREAT_VISIBLE');return false end
 if brain.router.pending or brain.router.nav.pending then return false end
 local here=p:getPosition();local j=brain.scavengeJob
 if not j then
  if now()-(brain.lastPlayerCorpseScan or -5000)<3000 then return false end
  brain.lastPlayerCorpseScan=now();brain.scavengeSkipped=brain.scavengeSkipped or{};local nearest=99
  for dx=-3,3 do for dy=-3,3 do local q={x=here.x+dx,y=here.y+dy,z=here.z};local tile=Tile(Position(q.x,q.y,q.z))
   if tile and not tile:getFieldItem()then for _,body in ipairs(tile:getItems()or{})do
    local guid=body:isContainer()and body:getCustomAttribute('crome_player_corpse_guid')
    if type(guid)=='number'and guid>0 then local k=key(q)..':'..guid;local claim=S.claims[k];local gap=math.max(math.abs(dx),math.abs(dy))
     if gap<nearest and(brain.scavengeSkipped[k]or 0)<now()and(not claim or claim.untilAt<now()or claim.owner==brain.a.name)then j={pos=q,key=k,guid=guid,at=now()};nearest=gap end
    end
   end end
  end end
  if not j then return false end
  brain.scavengeJob=j;brain.emit('PLAYER_CORPSE_DISCOVERED',{position=j.pos})
 end
 local body=j.body
 if j.guid then
  body=nil;local tile=Tile(Position(j.pos.x,j.pos.y,j.pos.z))
  for _,candidate in ipairs(tile and tile:getItems()or{})do if candidate:isContainer()and candidate:getCustomAttribute('crome_player_corpse_guid')==j.guid then body=candidate;break end end
 end
 if not body or now()-j.at>20000 or here.z~=j.pos.z then S.release(brain,'GONE_OR_EXPIRED');return false end
 local existing=S.claims[j.key]
 if existing and existing.owner~=brain.a.name and existing.untilAt>now()then S.release(brain,'CLAIMED_BY_OTHER');return false end
 S.claims[j.key]={owner=brain.a.name,untilAt=now()+4000}
 local item=nil;local best=0;walk(body,function(v)local score=useful(brain,p,v);if score>best then item=v;best=score end end,0)
 if not item then S.release(brain,'NO_USEFUL_ITEMS');return false end
 if math.max(math.abs(here.x-j.pos.x),math.abs(here.y-j.pos.y))>1 then
  local status=brain.router:move(p,j.pos,true)
  if status=='NO_ROUTE'or status=='BLOCKED'or status=='BUDGET_EXHAUSTED'then S.release(brain,status);return false end
  return true
 end
 local id=item:getId();local dest=brain.inventory:destination(p,id)
 if not dest then S.release(brain,'NO_SPACE');return false end
 local before=p:getItemCount(id);local sourceBefore=count(body,id);local ok,reason=p:cromeTakeLoot(item,dest)
 local gain=p:getItemCount(id)-before;local loss=sourceBefore-count(body,id)
 if gain>0 and gain==loss then brain.emit('PLAYER_LOOT_CONFIRMED',{id=id,count=gain,position=j.pos});brain.lastUseful=now();return true end
 if reason=='COOLDOWN'then return true end
 brain.emit('PLAYER_LOOT_REJECTED',{id=id,reason=tostring(reason),gain=gain,loss=loss});S.release(brain,'TRANSFER_NOT_CONFIRMED');return false
end
return S
