-- Five independent engine Players. Scenario objectives are test inputs, not learned routes.
local cfg=dofile('data/crome_batch_case.lua')
local prior=dofile('data/crome_batch_prior.lua')
local sessions={};local started=0;local running=false
local dirs={{0,-1,0},{1,0,1},{0,1,2},{-1,0,3}}
local function now()return os.mtime()end
local function cp(q)return{x=q.x,y=q.y,z=q.z}end
local function k(q)return q.x..','..q.y..','..q.z end
local function dist(a,b)if a.z~=b.z then return 999 end;return math.max(math.abs(a.x-b.x),math.abs(a.y-b.y))end
local function log(a,e,d)
 local line='[CROME_BATCH]|'..cfg.name..'|'..(a and a.name or '-')..'|'..e..'|'..(d or '')
 print(line);local f=io.open('data/crome_batch_events.log','a');if f then f:write(os.date('%Y-%m-%d %H:%M:%S')..' '..line..'\n');f:close()end
end
local function stop(a,reason)
 if a.done then return end;a.done=true
 local p=Player(a.id)
 if p then
  p:setTarget(nil);p:setFollowCreature(nil)
  log(a,'FINAL','reason='..reason..' hp='..p:getHealth()..' xpGain='..(p:getExperience()-a.xp)..' goldGain='..(p:getItemCount(2148)-a.gold)..' pos='..k(p:getPosition())..' moves='..a.moves..' minHp='..a.minHp..' kills='..a.kills..' loot='..a.loot..' steps='..a.goalIndex)
  for _,g in ipairs(cfg.goals or{})do if g.uid then
   local ground=0;local t=Tile(p:getPosition());for _,item in ipairs(t and t:getItems()or{})do if item:getId()==g.reward then ground=ground+item:getCount()end end
   log(a,'QUEST_FINAL','uid='..g.uid..' storage='..p:getStorageValue(g.uid)..' rewardCount='..p:getItemCount(g.reward)..' rewardOnOwnTile='..ground..' freeCap='..p:getFreeCapacity()..' bagSize='..p:getSlotItem(CONST_SLOT_BACKPACK):getSize())
  end end
  p:save();Game.stopCromeBot(a.id)
 else log(a,'FINAL','reason=REMOVED_OR_DEAD') end
end
local function route(p,goal,adjacent,a,frontier)
 local origin=cp(p:getPosition());local queue={{q=origin}};local seen={[k(origin)]=true};local head=1;local best=nil;local bestScore=999999
 while head<=#queue and head<=(frontier and 5000 or 200) do
  local n=queue[head];head=head+1
  if (adjacent and dist(n.q,goal)<=1)or(not adjacent and k(n.q)==k(goal))then return n.first end
  if n.first then local score=dist(n.q,goal)*100+(a.visits[k(n.q)]or 0);if score<bestScore then best=n.first;bestScore=score end end
  for _,d in ipairs(dirs)do local q={x=n.q.x+d[1],y=n.q.y+d[2],z=n.q.z}
   local live=math.abs(q.x-origin.x)<=7 and math.abs(q.y-origin.y)<=5
   local pass=false
   if live then pass=p:cromeCanStep(Position(q.x,q.y,q.z))
   elseif frontier then local remembered=a.known[k(q)];if remembered~=nil then pass=remembered else pass=prior[k(q)]==true end end
   if not seen[k(q)] and math.abs(q.x-origin.x)<=(frontier and 40 or 7) and math.abs(q.y-origin.y)<=(frontier and 40 or 5) and pass then seen[k(q)]=true;queue[#queue+1]={q=q,first=n.first or{q=q,d=d[3]}}end
  end
 end
 if frontier then return best end
 return nil
end
local function move(p,a,s)
 if not s then return false end
 if p:cromeStep(s.d)then a.pending={from=cp(p:getPosition()),to=s.q,at=now()};return true end
 return false
end
local function object(q,uid,aid,id)
 local tile=Tile(Position(q.x,q.y,q.z));if not tile then return end
 local items=tile:getItems()or{};local ground=tile:getGround();if ground then items[#items+1]=ground end
 for _,v in ipairs(items)do if (not uid or v:getUniqueId()==uid)and(not aid or v:getActionId()==aid)and(not id or v:getId()==id)then return v end end
end
local function feed(p,a)
 if now()-(a.fedAt or 0)<2000 then return end
 local c=p:getCondition(CONDITION_REGENERATION,CONDITIONID_DEFAULT);if c and c:getTicks()>30000 then return end
 for _,id in ipairs({2696,2666,2674,2677})do local item=p:getItemById(id,true);if item then local n=p:getItemCount(id);p:cromeUseFood(item);a.fedAt=now();if p:getItemCount(id)<n then log(a,'FOOD','id='..id)end;return end end
end
local function loot(p,a)
 if not a.body then return false end
 if now()-a.body.at>10000 then log(a,'LOOT_TIMEOUT');a.body=nil;return false end
 local here=p:getPosition()
 for dx=-2,2 do for dy=-2,2 do
  local q={x=a.body.pos.x+dx,y=a.body.pos.y+dy,z=a.body.pos.z}
  if math.abs(q.x-here.x)<=7 and math.abs(q.y-here.y)<=5 then
   local tile=Tile(Position(q.x,q.y,q.z))
   for _,body in ipairs(tile and tile:getItems()or{})do
    if body:isContainer() and body:getCorpseOwner()==a.id then
     if dist(here,q)>1 then return move(p,a,route(p,q,true,a)) end
     for i=0,body:getSize()-1 do local item=body:getItem(i);local id=item:getId()
      if id==2148 or(id==2696 and p:getItemCount(id)<2)or(id==2666 and p:getItemCount(id)<2)then
       local before=p:getItemCount(id);local accepted,reason=p:cromeTakeLoot(item,p:getSlotItem(CONST_SLOT_BACKPACK));local gain=p:getItemCount(id)-before
       if gain>0 then a.loot=a.loot+gain;log(a,'LOOT_CONFIRMED','id='..id..' gain='..gain)
       elseif reason~='COOLDOWN' then log(a,'LOOT_REJECTED','id='..id..' reason='..tostring(reason));a.body=nil end
       return true
      end
     end
     a.body=nil;return true
    end
   end
  end
 end end
 return true
end
local function tick(a)
 if a.done then return end
 local p=Player(a.id);if not p or p:isRemoved()then stop(a,'REMOVED_OR_DEAD');return end
 p:cromeHeartbeat();a.minHp=math.min(a.minHp,p:getHealth())
 if p:getHealth()<math.max(45,p:getMaxHealth()*.35)then stop(a,'HP_FLOOR');return end
 if p:getLevel()>=8 then stop(a,'LEVEL_8');return end
 if a.pending then
  local here=p:getPosition()
  if k(here)==k(a.pending.to)then a.moves=a.moves+1;a.progressAt=now();a.visits[k(here)]=(a.visits[k(here)]or 0)+1;a.pending=nil
  elseif k(here)~=k(a.pending.from)then stop(a,'UNEXPECTED_DISPLACEMENT');return
  elseif now()-a.pending.at>3000 then a.pending=nil;log(a,'STEP_UNCERTAIN')else return end
 end
 if now()-(a.reportAt or 0)>10000 then a.reportAt=now();log(a,'PROGRESS','pos='..k(p:getPosition())..' hp='..p:getHealth()..' goal='..a.goalIndex..' target='..tostring(a.target and a.target.id))end
 feed(p,a)
 local here=p:getPosition();local enemies={}
 if cfg.mode=='quest' then
  for dx=-7,7 do for dy=-5,5 do
   local q={x=here.x+dx,y=here.y+dy,z=here.z};local tile=Tile(Position(q.x,q.y,q.z))
   if tile and not tile:getTopCreature() then a.known[k(q)]=p:cromeCanStep(Position(q.x,q.y,q.z))end
  end end
 end
 for _,c in ipairs(Game.getSpectators(here,false,false,7,7,5,5))do
  if c:isMonster() and c:getType():isHostile()and c:getPosition().z==here.z then enemies[#enemies+1]=c end
 end
 if cfg.mode=='hunt' then
 if a.target then
  local target=Creature(a.target.id)
  if not target or target:isRemoved()then
   if p:getExperience()>a.target.xp then a.kills=a.kills+1;a.body={pos=a.target.pos,at=now()};log(a,'XP_CONFIRMED','gain='..(p:getExperience()-a.target.xp))end
   p:setTarget(nil);a.target=nil
  elseif target:getPosition().z~=here.z or dist(here,target:getPosition())>7 then p:setTarget(nil);a.target=nil
  else
   if target:getHealth()<a.target.hp then a.target.hp=target:getHealth();a.target.at=now()end
   a.target.pos=cp(target:getPosition())
   if now()-a.target.at>20000 then p:setTarget(nil);a.skipped[a.target.id]=now()+20000;a.target=nil;log(a,'TARGET_NO_PROGRESS')
   else
    p:setTarget(target)
    if dist(here,target:getPosition())>1 then move(p,a,route(p,target:getPosition(),true,a))end
    return
   end
  end
 end
 if a.body and loot(p,a)then return end
 for _,c in ipairs(enemies)do
  local focus=c:getTarget();local own=not focus or not focus:isPlayer()or focus:getId()==a.id
  local relevant=(cfg.mode=='hunt'and c:getName():lower()==cfg.monster)or dist(here,c:getPosition())<=1
  if own and relevant and(not a.skipped[c:getId()]or a.skipped[c:getId()]<now())then
   local path=route(p,c:getPosition(),true,a)
   if dist(here,c:getPosition())<=1 or path then a.target={id=c:getId(),xp=p:getExperience(),pos=cp(c:getPosition()),hp=c:getHealth(),at=now()};log(a,'TARGET','id='..c:getId()..' name='..c:getName());p:setTarget(c);return end
  end
 end
 else
  for _,c in ipairs(enemies)do if dist(here,c:getPosition())<=1 and here:isSightClear(c:getPosition(),true) then p:setTarget(c);break end end
 end
 if cfg.mode=='hunt'then
  -- A visible bridge switch is an interaction candidate if no target is reachable.
  for dx=-1,1 do for dy=-1,1 do local q={x=here.x+dx,y=here.y+dy,z=here.z};local lever=object(q,nil,3100,1945)
   if lever then local ok=p:cromeUseObject(lever);if ok then log(a,'BRIDGE_USED','pos='..k(q));return end end
  end end
  local choices={};for _,d in ipairs(dirs)do local q={x=here.x+d[1],y=here.y+d[2],z=here.z};if p:cromeCanStep(Position(q.x,q.y,q.z))and dist(q,a.anchor)<=18 then choices[#choices+1]={q=q,d=d[3],score=(a.visits[k(q)]or 0)}end end
  table.sort(choices,function(x,y)return x.score<y.score end);move(p,a,choices[1]);return
 end
 local goal=cfg.goals[a.goalIndex]
 if not goal then stop(a,'OBJECTIVES_COMPLETED');return end
 if math.abs(here.x-goal.pos.x)<=7 and math.abs(here.y-goal.pos.y)<=5 and here.z==goal.pos.z and goal.kind=='lever' then
  local visible=object(goal.pos,goal.uid,goal.aid,goal.id)
  if visible and visible:getId()==1946 then log(a,'SHARED_LEVER_OBSERVED','aid='..goal.aid);a.goalIndex=a.goalIndex+1;return end
 end
 if math.abs(here.x-goal.pos.x)<=7 and math.abs(here.y-goal.pos.y)<=5 and here.z==goal.pos.z and goal.kind=='door' then
  local visible=object(goal.pos,goal.uid,goal.aid,goal.id)
  if visible and visible:getId()==1214 then log(a,'SHARED_DOOR_OBSERVED','id='..visible:getId());a.goalIndex=a.goalIndex+1;return end
 end
 if dist(here,goal.pos)>1 then
  local path=route(p,goal.pos,true,a,true)
  if not path or(now()-(a.progressAt or a.started)>6000)then
   local choices={};for _,d in ipairs(dirs)do local q={x=here.x+d[1],y=here.y+d[2],z=here.z};if p:cromeCanStep(Position(q.x,q.y,q.z))then choices[#choices+1]={q=q,d=d[3],score=(a.visits[k(q)]or 0)}end end
   table.sort(choices,function(x,y)return x.score<y.score end)
   if choices[1]then move(p,a,choices[1]);a.progressAt=now();log(a,'YIELD_PATH');return end
  end
  move(p,a,path);return
 end
 local item=object(goal.pos,goal.uid,goal.aid,goal.id)
 if not item then log(a,'OBJECT_MISSING','goal='..a.goalIndex);stop(a,'OBJECT_MISSING');return end
 if a.retryAt and now()<a.retryAt then return end;a.retryAt=now()+600
 if goal.kind=='chest'then
  if p:getStorageValue(goal.uid)==-1 then
   local weight=0;for i=0,item:getSize()-1 do weight=weight+item:getItem(i):getWeight()end
   if p:getFreeCapacity()<weight then log(a,'CAPACITY_REJECTED','free='..p:getFreeCapacity()..' needed='..weight);stop(a,'CAPACITY_LIMIT');return end
  end
  if p:getStorageValue(goal.uid)~=-1 then
   if p:getItemCount(goal.reward)>a.rewardBefore[goal.uid]then
    local before=p:getItemCount(goal.reward);local accepted,why=p:cromeUseObject(item)
    if not accepted and why=='COOLDOWN' then return end
    if p:getItemCount(goal.reward)~=before then stop(a,'DUPLICATE_QUEST_REWARD');return end
    log(a,'QUEST_REPEAT_NO_DUPLICATE','uid='..goal.uid)
    log(a,'QUEST_CONFIRMED','uid='..goal.uid..' item='..goal.reward);a.goalIndex=a.goalIndex+1
   else stop(a,'STORAGE_WITHOUT_REWARD')end
   return
  end
  a.chestAttempts=a.chestAttempts or{}
  if (a.chestAttempts[goal.uid]or 0)>=2 and p:getSlotItem(CONST_SLOT_BACKPACK):getCapacity()-p:getSlotItem(CONST_SLOT_BACKPACK):getSize()<item:getSize() then stop(a,'INVENTORY_SPACE_LIMIT');return end
  local accepted,why=p:cromeUseObject(item);if accepted then a.chestAttempts[goal.uid]=(a.chestAttempts[goal.uid]or 0)+1 end;log(a,'CHEST_SENT','uid='..goal.uid..' accepted='..tostring(accepted)..' reason='..tostring(why));return
 elseif goal.kind=='lever'then
  if item:getId()==1946 then log(a,'LEVER_STATE_OBSERVED','aid='..goal.aid);a.goalIndex=a.goalIndex+1;return end
  p:cromeUseObject(item);log(a,'LEVER_SENT','aid='..goal.aid);return
 elseif goal.kind=='door'then
  if item:getId()~=1212 then log(a,'DOOR_CHANGED','id='..item:getId());a.goalIndex=a.goalIndex+1;return end
  if not a.triedWithoutKey then a.triedWithoutKey=true;p:cromeUseObject(item);log(a,'LOCKED_DOOR_PROBE','id='..item:getId());return end
  local keyItem=p:getItemById(2089,true)
  if not keyItem or keyItem:getActionId()~=goal.aid then stop(a,'MATCHING_KEY_MISSING');return end
  local ok,why=p:cromeUseObject(keyItem,item);log(a,'KEY_SENT','accepted='..tostring(ok)..' reason='..tostring(why));return
 end
end
local event=GlobalEvent('CromeFivePlayerBatch')
function event.onStartup()
 addEvent(function()
  started=now();running=true
  local q=cfg.starts[1];local t=Tile(Position(q.x,q.y,q.z))
  log(nil,'START_TILE_AUDIT','pos='..k(q)..' exists='..tostring(t~=nil)..' ground='..tostring(t and t:getGround() and t:getGround():getId()))
  for _,g in ipairs(cfg.goals or{})do
   local t=Tile(Position(g.pos.x,g.pos.y,g.pos.z));log(nil,'GOAL_TILE_AUDIT','pos='..k(g.pos)..' exists='..tostring(t~=nil)..' ground='..tostring(t and t:getGround()and t:getGround():getId()))
  end
  for i=1,5 do
   local name='Crome Teste '..i;local q=cfg.starts[i]or cfg.starts[1]
   local id,err=Game.startCromeBot(name,Position(q.x,q.y,q.z))
   if not id and tostring(err):find('No free safe spawn',1,true) then
    for radius=1,6 do
     for dx=-radius,radius do for dy=-radius,radius do
      if not id and(math.abs(dx)==radius or math.abs(dy)==radius)then id,err=Game.startCromeBot(name,Position(q.x+dx,q.y+dy,q.z))end
     end end
     if id then break end
    end
   end
   if id then
    local p=Player(id);local a={id=id,name=name,started=now(),xp=p:getExperience(),gold=p:getItemCount(2148),minHp=p:getHealth(),anchor=cp(p:getPosition()),moves=0,kills=0,loot=0,visits={},skipped={},known={},goalIndex=1,rewardBefore={}}
    for _,g in ipairs(cfg.goals or{})do if g.uid then a.rewardBefore[g.uid]=p:getItemCount(g.reward)end end
    sessions[#sessions+1]=a;log(a,'START','hp='..p:getHealth()..' pos='..k(p:getPosition()))
   else log({name=name},'START_FAILED',tostring(err))end
  end
  log(nil,'CONCURRENT_COUNT','count='..#sessions)
  if #sessions==5 then
  local extra,why=Game.startCromeBot('Crome Interno',Position(cfg.starts[1].x,cfg.starts[1].y,cfg.starts[1].z))
  if extra then log(nil,'SIXTH_LIMIT_FAILED');Game.stopCromeBot(extra)else log(nil,tostring(why):find('five players',1,true) and 'SIXTH_LIMIT_REJECTED' or 'SIXTH_UNEXPECTED_REJECTION',tostring(why))end
  end
  local function loop()
   local active=0
   for _,a in ipairs(sessions)do if not a.done then active=active+1;local ok,err=xpcall(function()tick(a)end,debug.traceback);if not ok then log(a,'ERROR',err);stop(a,'SCRIPT_ERROR')end end end
   if active==0 or now()-started>cfg.seconds*1000 then
    for _,a in ipairs(sessions)do stop(a,'CASE_TIMEOUT')end
    for _,g in ipairs(cfg.goals or{})do local item=object(g.pos,nil,g.aid,nil);if item then log(nil,'FINAL_OBJECT_STATE','pos='..k(g.pos)..' id='..item:getId()..' aid='..item:getActionId())end end
    log(nil,'NORMAL_SHUTDOWN');Game.setGameState(GAME_STATE_SHUTDOWN);return
   end
   addEvent(loop,250)
  end
  loop()
 end,3000)
 return true
end
event:register()
