local cfg=dofile('data/crome_race/config.lua')
if not cfg.validatePlayerLoot then return end
local event=GlobalEvent('CromePlayerLootPreflight')
function event.onStartup()
 local function test(attempt)
  for _,p in ipairs(Game.getPlayers())do
   local safe=true;for _,c in ipairs(Game.getSpectators(p:getPosition(),false,false,7,7,5,5))do if c:isMonster()and c:getType():isHostile()then safe=false;break end end
   if p:isCromeInternalBot()and safe then
    local dest=p:getSlotItem(3)
    if dest and dest:isContainer()and dest:getSize()<dest:getCapacity()then
     local body=Game.createItem(3058,1,p:getPosition());local other=Game.createItem(1988,1,p:getPosition())
     local before=p:getItemCount(2148);local gain=0
     local ok,err=pcall(function()
      body:setCustomAttribute('crome_player_corpse_guid',p:getGuid())
      local bag=body:addItem(1988,1);local coins=bag:addItem(2148,3)
      local S=dofile('data/crome_race/scavenge.lua');local confirmed=false
      local brain={a={name='LOOT_FIXTURE'},router={nav={}},wiki={items={},foods={}},inventory={slot=function()return nil end,destination=function()return dest end},emit=function(kind)if kind=='PLAYER_LOOT_CONFIRMED'then confirmed=true end end,scavengeJob={body=body,pos={x=p:getPosition().x,y=p:getPosition().y,z=p:getPosition().z},key='fixture:'..p:getGuid(),at=os.mtime()}}
      S.tick(brain,p,{});S.release(brain,'FIXTURE_END')
      local taken=confirmed;local why='TRANSFER_NOT_CONFIRMED'
      gain=p:getItemCount(2148)-before
      assert(taken and gain==3 and bag:getSize()==0,'Nested player corpse transfer failed')
      local foreign=other:addItem(2148,1)
      assert(not p:cromeTakeLoot(foreign,dest),'Ordinary ground container must remain inaccessible')
      assert(not p:cromeTakeLoot(dest:getItem(0),other),'Destination outside player must be rejected')
     end)
     gain=p:getItemCount(2148)-before
     if gain>0 then assert(p:removeItem(2148,gain),'Fixture money cleanup failed')end
     body:remove();other:remove()
     assert(p:getItemCount(2148)==before,'Fixture must preserve player coins')
     if not ok then print('[CROME_LOOT_PREFLIGHT] FAIL '..tostring(err));return end
     if gain==3 then print('[CROME_LOOT_PREFLIGHT] PASS autonomous loot module + native nested player corpse transfer; ordinary container rejected; foreign destination rejected; fixture cleaned');return end
    end
   end
  end
  if attempt<20 then addEvent(function()test(attempt+1)end,500)else print('[CROME_LOOT_PREFLIGHT] NOT_RUN no suitable player')end
 end
 addEvent(function()test(1)end,5000)
 return true
end
event:register()
