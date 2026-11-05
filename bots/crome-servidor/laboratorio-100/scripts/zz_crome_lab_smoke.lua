-- Autonomous race: persistent actors, native combat/shops/movement and normal XP.
local cfg=dofile('data/crome_race/config.lua')
local hasLevelGoal=(cfg.noLevelGoal==false)and cfg.goalLevel and cfg.goalLevel>0 and cfg.goalLevel<999
local resetBootDone=false
local expectedOnline=0;for _,name in ipairs(cfg.names)do if not(cfg.offlinePlayers and cfg.offlinePlayers[name])then expectedOnline=expectedOnline+1 end end
local LabFood=dofile('data/crome_race/food_test.lua')
local wiki=dofile('data/crome_race/wiki.lua')
local caveHints={entrances={},links={}}
for _,hint in ipairs(caveHints.entrances)do local found=false;for _,known in ipairs(wiki.entrances)do if known.pos.x==hint.pos.x and known.pos.y==hint.pos.y and known.pos.z==hint.pos.z then found=true;break end end;if not found then table.insert(wiki.entrances,hint)end end
wiki.caveLinks=wiki.caveLinks or caveHints.links

local trainingSpecies={rat=true,['cave rat']=true,bug=true,spider=true,wolf=true,snake=true,troll=true,skeleton=true}
-- Spawn seconds already match the current native server XML.
local prior=dofile('data/crome_race/map_prior.lua')
local Inventory=dofile('data/crome_race/inventory.lua')
local Brain=dofile('data/crome_race/baseline/brain.lua')
local CandidateBrain=dofile('data/crome_race/candidate/brain.lua')
local CandidateInventory=dofile('data/crome_race/candidate/inventory.lua')
local candidateInventory=CandidateInventory.new(wiki)
CandidateBrain.Market.leases=Brain.Market.leases
local Nav=dofile('data/crome_race/navigation.lua')
local inventory=Inventory.new(wiki)
local nativeVerified=false;local buckets=math.max(5,math.ceil(cfg.players/4));local actors={};local byId={};local running=false;local shutting=false;local raceStart=nil;local bucket=0;local lastBoard=0;local lastLoop=nil
local lastMotion=0
local function now()return os.mtime()end
local elapsedBefore=0
local clockFile=io.open(cfg.clockPath,'r')
if clockFile then local header=clockFile:read('*l');if header=='CROME_RACE_CLOCK_1\t'..cfg.domain then elapsedBefore=tonumber(clockFile:read('*l'))or 0 end;clockFile:close()end
local function raceElapsed()return elapsedBefore+(raceStart and now()-raceStart or 0)end
local function saveClock()pcall(Nav.atomic,cfg.clockPath,'CROME_RACE_CLOCK_1\t'..cfg.domain..'\n'..math.floor(raceElapsed())..'\n')end
local function quote(s)return '"'..s:gsub('\\','\\\\'):gsub('"','\\"'):gsub('\n','\\n'):gsub('\r','\\r'):gsub('\t','\\t')..'"'end
local function json(v)
 if v==nil then return 'null'end
 if type(v)=='boolean'or type(v)=='number'then return tostring(v)end
 if type(v)=='string'then return quote(v)end
 if type(v)~='table'then return quote(tostring(v))end
 local arr=#v>0;local out={}
 if arr then for i=1,#v do out[#out+1]=json(v[i])end;return '['..table.concat(out,',')..']'end
 local keys={};for k in pairs(v)do keys[#keys+1]=k end;table.sort(keys)
 for _,k in ipairs(keys)do out[#out+1]=quote(tostring(k))..':'..json(v[k])end
 return '{'..table.concat(out,',')..'}'
end
local eventLog=nil;local lastLogFlush=0
local function emit(a,event,data)
 if type(data)~='table'then data={detail=tostring(data)}end
 local record={at=os.time(),ms=now(),actor=a and a.name or'RACE',variant=a and a.variant or'COMMON',event=event,data=data or{}}
 local line=json(record)
 if not eventLog then eventLog=io.open(cfg.eventPath,'a')or assert(io.open('data/crome_race/events.jsonl','a'));eventLog:setvbuf('full',65536)end
 eventLog:write(line..'\n')
 if event~='MOVE_COMPLETED'then print('[CROME_RACE] '..line)end
end
local function resetAllPlayersToLevel1()
 local tx,ty,tz=cfg.temple.x,cfg.temple.y,cfg.temple.z
 local sql=string.format(
  'UPDATE `players` SET `level`=1,`experience`=0,`health`=150,`healthmax`=150,`mana`=0,`manamax`=0,`maglevel`=0,`manaspent`=0,`cap`=400,`soul`=100,`posx`=%d,`posy`=%d,`posz`=%d WHERE `name` LIKE %s OR `name` LIKE %s',
  tx,ty,tz,db.escapeString('Crome%'),db.escapeString('Aventureiro%')
 )
 local ok=db.query(sql)
 elapsedBefore=0
 saveClock()
 resetBootDone=true
 emit(nil,'RESET_ALL_TO_LEVEL_1',{ok=ok and true or false,temple=cfg.temple,noLevelGoal=not hasLevelGoal})
end
local function ensureSpawnableInDb(a)
 -- C++ luaGameStartCromeBot rejects characters if DB level >= 8 or health <= 0 at login/respawn time.
 if not db or not db.storeQuery then return end
 local q=db.storeQuery('SELECT `id`,`level`,`experience`,`health`,`healthmax` FROM `players` WHERE `name`='..db.escapeString(a.name)..' LIMIT 1')
 if not q then return end
 local pid=result.getNumber(q,'id')
 local lvl=result.getNumber(q,'level')
 local xp=result.getNumber(q,'experience')
 local hp=result.getNumber(q,'health')
 local maxhp=math.max(150,result.getNumber(q,'healthmax'))
 result.free(q)
 if resetBootDone and not a.spawnedOnce then
  db.query(string.format('UPDATE `players` SET `level`=1,`experience`=0,`health`=150,`healthmax`=150,`mana`=0,`manamax`=0,`posx`=%d,`posy`=%d,`posz`=%d WHERE `id`=%d',cfg.temple.x,cfg.temple.y,cfg.temple.z,pid))
  a.savedTrueLevel=nil;a.savedTrueXp=nil
 elseif lvl>=8 or hp<=0 then
  a.savedTrueLevel=(lvl>=8)and lvl or nil
  a.savedTrueXp=(lvl>=8)and xp or nil
  local safeLvl=math.min(7,math.max(1,lvl))
  db.query(string.format('UPDATE `players` SET `level`=%d,`health`=%d,`healthmax`=%d WHERE `id`=%d',safeLvl,maxhp,maxhp,pid))
 end
end
local function snapshot(a)
 local p=a.id and Player(a.id)
 if not p then return a.final or{name=a.name,status=a.status or'WAITING',index=a.index}end
 local q=p:getPosition();local s=a.brain and a.brain.stats or{}
 return{name=a.name,index=a.index,variant=a.variant,arm=cfg.armByName and cfg.armByName[a.name]or nil,baselineXp=a.measure and a.measure.xp or 0,baselineMoney=a.measure and a.measure.money or 0,damageTaken=a.measure and a.measure.damage or 0,status=a.status or'ACTIVE',level=p:getLevel(),xp=p:getExperience(),hp=p:getHealth(),maxHp=p:getMaxHealth(),money=p:getMoney(),bank=p.getBankBalance and p:getBankBalance()or 0,position={x=q.x,y=q.y,z=q.z},tickMaxMillis=a.tickMax or 0,tickMeanMillis=(a.tickSum or 0)/math.max(1,a.tickCount or 0),goal=a.brain and a.brain.goal and a.brain.goal.id or'NONE',playedMillis=s.played or 0,deaths=s.deaths or 0,earned=s.earned or 0,spent=s.spent or 0,moves=s.moves or 0,transitions=s.transitions or 0,metas=s.metas or 0,uncertain=s.uncertain or 0,loops=s.loops or 0,xpPerMinute=(s.autonomyXp or 0)*60000/math.max(1,s.autonomyMillis or 0),idlePercent=(s.autonomyIdle or 0)*100/math.max(1,s.autonomyMillis or 0),goalsFiltered=a.brain and a.brain.selectionStats.filtered or 0,regionValue=a.brain and a.brain.memory:value(q)or 0,checkpoint3=s.checkpoint3,finishedMillis=hasLevelGoal and s.finishedGoal==cfg.goalLevel and s.finished or nil}
end
local lastOverlapLog=0
local function board(final)
 local rows={};local active=0
 if now()-lastOverlapLog>15000 then
  local occupied={}
  for _,a in ipairs(actors)do local p=a.id and Player(a.id);if p and not p:isRemoved()then local q=p:getPosition();local k=q.x..','..q.y..','..q.z;occupied[k]=occupied[k]or{};table.insert(occupied[k],a.name)end end
  for pos,names in pairs(occupied)do if #names>1 then emit(nil,'BOT_PASS_THROUGH_OBSERVED',{position=pos,players=names,scope='INTERNAL_BOTS_ONLY'});lastOverlapLog=now();break end end
 end
 for _,a in ipairs(actors)do local row=snapshot(a);rows[#rows+1]=row;if not a.done then active=active+1 end end
 table.sort(rows,function(a,b)
  if a.finishedMillis or b.finishedMillis then if not a.finishedMillis then return false elseif not b.finishedMillis then return true else return a.finishedMillis<b.finishedMillis end end
  if(a.level or 0)~=(b.level or 0)then return(a.level or 0)>(b.level or 0)end
  if(a.xp or 0)~=(b.xp or 0)then return(a.xp or 0)>(b.xp or 0)end
  if(a.deaths or 0)~=(b.deaths or 0)then return(a.deaths or 0)<(b.deaths or 0)end
  return a.index<b.index
 end)
 local online=0;for _,a in ipairs(actors)do local p=a.id and Player(a.id);if p and not p:isRemoved()and p:getHealth()>0 then online=online+1 end end
 local data={schema=1,at=os.time(),active=active,online=online,controllerVersion='lab100-foodinf-regen2-motion',motionMode='CACHED_PATH_NATIVE_COOLDOWN',experimentCondition=cfg.experimentCondition,experimentRound=cfg.experimentRound,controllerBuild=cfg.controllerBuild,goalLevel=hasLevelGoal and cfg.goalLevel or 0,noLevelGoal=not hasLevelGoal,checkpointLevel=cfg.checkpointLevel,elapsedOverall=raceElapsed(),elapsedThisSegment=raceStart and now()-raceStart or 0,secondsLimit=cfg.seconds,xpRules='STANDARD_100_200_400',final=final or false,players=rows}
 saveClock()
 local ok,why=Nav.atomic(cfg.boardPath,json(data)..'\n');if not ok then emit(nil,'BOARD_WRITE_ERROR',{error=tostring(why)})end
end
local function stop(a,reason)
 if a.done then return end
 local p=a.id and Player(a.id)
 if p then
  if a.brain then
   if a.trade then Brain.Market.release(a,p)end
   local ok,why=pcall(function()a.brain:save(p,true)end);if not ok then emit(a,'SAVE_ERROR',{error=tostring(why)})end
  end
  a.final=snapshot(a);a.final.status=reason;p:setTarget(nil);p:setFollowCreature(nil);p:save();Game.stopCromeBot(a.id)
 else a.final=a.final or{name=a.name,index=a.index,status=reason}end
 if a.id then byId[a.id]=nil end
 a.done=true;a.status=reason;emit(a,'ACTOR_STOPPED',a.final)
end
local function completedOfflineProfile(a)
 if not hasLevelGoal or resetBootDone then return nil end
 local guid=cfg.playerGuids and cfg.playerGuids[a.name];if not guid then return end
 local q=db.storeQuery('SELECT level,experience,health,healthmax,posx,posy,posz FROM players WHERE id='..guid..' AND name='..db.escapeString(a.name)..' LIMIT 1')
 if not q then return end
 local level=result.getNumber(q,'level');local row=nil
 if level>=cfg.goalLevel then
  row={name=a.name,index=a.index,variant=a.variant,arm=cfg.armByName and cfg.armByName[a.name]or nil,level=level,xp=result.getNumber(q,'experience'),baselineXp=result.getNumber(q,'experience'),hp=result.getNumber(q,'health'),maxHp=result.getNumber(q,'healthmax'),position={x=result.getNumber(q,'posx'),y=result.getNumber(q,'posy'),z=result.getNumber(q,'posz')},status='GOAL_LEVEL'..cfg.goalLevel,goal='GOAL_REACHED',deaths=0,completedBeforeThisSegment=true}
 end
 result.free(q);return row
end
local function attach(a)
 ensureSpawnableInDb(a)
 local id,why=Game.startCromeBot(a.name,Position(cfg.temple.x,cfg.temple.y,cfg.temple.z),true)
 if not id then a.startError=tostring(why);return false end
 a.spawnedOnce=true
 a.id=id;byId[id]=a;local p=Player(id)
 if cfg.requireFreshAtBoot and not raceStart then assert(p:getLevel()==1 and p:getExperience()==0,'Fresh level1 profile required');assert(p:getGuid()==cfg.playerGuids[a.name],'Wrong player identity')end
 p:registerEvent('CromeRace20Death');p:registerEvent('CromeRace20Advance');p:registerEvent('CromeABDamage')
 a.measure=a.measure or{xp=p:getExperience(),money=p:getMoney(),damage=0}
 local useCandidate=(a.variant=='candidate')or(a.variant=='tibinha')or(cfg.armByName~=nil);local controller=useCandidate and CandidateBrain or Brain;local inv=useCandidate and candidateInventory or inventory
 LabFood.ensure(p,inv,function(event,data)emit(a,event,data)end)
 local ok,b=pcall(controller.new,a,cfg,wiki,prior,inv,function(e,d)emit(a,e,d)end)
 if not ok then emit(a,'BRAIN_START_ERROR',{error=tostring(b)});Game.stopCromeBot(id);byId[id]=nil;a.id=nil;return false end
 if resetBootDone or not hasLevelGoal then
  b.stats.finished=nil;b.stats.finishedGoal=nil
  if resetBootDone then b.stats.checkpoint3=nil end
 end
 a.brain=b;a.status='ACTIVE';a.done=false;a.deadAt=nil;a.lastSave=now()-((a.index*977)%60000)
 emit(a,'START',{index=a.index,level=p:getLevel(),xp=p:getExperience(),hp=p:getHealth(),position=snapshot(a).position,continued=not(cfg.requireFreshAtBoot or resetBootDone),arm=cfg.armByName and cfg.armByName[a.name]or nil,historicalKnowledge=true,rawQImported=false})
 return true
end
local function verifyPopulation()
 if nativeVerified then return end
 local live=0;for _,a in ipairs(actors)do if a.id and Player(a.id)then live=live+1 end end
 if live==expectedOnline and expectedOnline<cfg.players then nativeVerified=true;emit(nil,'CONCURRENT_ACTORS',{count=live,requested=expectedOnline,excluded=cfg.players-expectedOnline});return end
 if live==cfg.players then
  local extra,reason=Game.startCromeBot('Crome Interno',Position(cfg.temple.x,cfg.temple.y,cfg.temple.z))
  assert(not extra,'Native player limit failed')
  local lower=Game.setCromeBotLimit(cfg.players-1);assert(not lower,'Limit below active players must fail')
  nativeVerified=true;emit(nil,'CONCURRENT_ACTORS',{count=live,requested=cfg.players});emit(nil,'LIMIT101_REJECTED',{reason=tostring(reason)});emit(nil,'LIMIT_ACTIVE_VALIDATED',{active=live})
 end
end
local death=CreatureEvent('CromeRace20Death')
function death.onDeath(player,corpse,killer,mostDamageKiller,...)
 local a=byId[player:getId()];if not a then return true end
 a.deadAt=now();a.status='DEAD_WAITING';a.final=snapshot(a);a.final.status='DEAD_WAITING'
 if a.brain then
  a.brain:markDanger(player:getPosition(),killer and killer:getName()or 'unknown')
  a.brain.stats.deaths=a.brain.stats.deaths+1
  a.brain.memory:death(player:getPosition())
  if a.brain.policy.pending then a.brain.policy:reward('DEATH',-30)end
  pcall(function()a.brain:save(player,true)end)
 end
 emit(a,'DEATH_CONFIRMED',{killer=killer and killer:getName()or'UNKNOWN',position=a.final.position,xp=player:getExperience(),money=player:getMoney()})
 return true
end
death:register()
local incoming=CreatureEvent('CromeABDamage')
function incoming.onHealthChange(player,attacker,primaryDamage,primaryType,secondaryDamage,secondaryType,origin)
 local a=byId[player:getId()]
 if a and attacker and attacker:isMonster()and primaryType~=COMBAT_HEALING then
  local amount=math.abs(primaryDamage)+math.abs(secondaryDamage)
  if amount>0 then
   if a.brain then a.brain.fastNeedsThink=true end
   a.measure.damage=a.measure.damage+math.min(amount,math.max(0,player:getHealth()))
   emit(a,'INCOMING_DAMAGE',{monster=attacker:getName(),amount=amount,hp=player:getHealth(),position={x=player:getPosition().x,y=player:getPosition().y,z=player:getPosition().z}})
   if a.brain and a.brain.observedDamage then a.brain:observedDamage(attacker,amount)end
  end
 end
 return primaryDamage,primaryType,secondaryDamage,secondaryType
end
incoming:register()
local outgoing=CreatureEvent('CromeABOutgoing')
function outgoing.onHealthChange(monster,attacker,primaryDamage,primaryType,secondaryDamage,secondaryType,origin)
 local a=attacker and attacker:isPlayer()and byId[attacker:getId()]
 if a and primaryType~=COMBAT_HEALING then
  local amount=math.abs(primaryDamage)+math.abs(secondaryDamage)
  if amount>0 then emit(a,'OUTGOING_DAMAGE',{monster=monster:getName(),amount=amount,targetHp=monster:getHealth()})end
 end
 return primaryDamage,primaryType,secondaryDamage,secondaryType
end
outgoing:register()
local advance=CreatureEvent('CromeRace20Advance')
function advance.onAdvance(player,skill,old,new)
 local a=byId[player:getId()];if not a or skill~=SKILL_LEVEL then return true end
 local elapsed=raceElapsed();saveClock()
 if new>old then emit(a,'LEVEL_UP',{oldLevel=old,level=new,elapsedMillis=elapsed,xp=player:getExperience()})end
 if new>=cfg.checkpointLevel and not a.brain.stats.checkpoint3 then a.brain.stats.checkpoint3=elapsed;emit(a,'CHECKPOINT_LEVEL3',{level=new,elapsedMillis=elapsed,xp=player:getExperience()})end
 if hasLevelGoal and new>=cfg.goalLevel and(not a.brain.stats.finished or a.brain.stats.finishedGoal~=cfg.goalLevel)then a.brain.stats.finished=elapsed;a.brain.stats.finishedGoal=cfg.goalLevel;emit(a,'FINISH_LEVEL'..cfg.goalLevel,{elapsedMillis=elapsed,xp=player:getExperience()})end
 if a.brain.policy.pending then a.brain.policy:reward('LEVEL_GAIN',(new-old)*5)end
 return true
end
advance:register()
local function shutdown(reason)
 if shutting then return end;shutting=true
 emit(nil,'STOP_REQUESTED',{reason=reason})
 for _,a in ipairs(actors)do stop(a,reason)end
 if eventLog then eventLog:flush();eventLog:close();eventLog=nil end
 board(true);Game.setGameState(GAME_STATE_SHUTDOWN)
end
local function step(a)
 if a.done then return end
 if a.spawnRetry then
  if not a.lastSpawnRetry or now()-a.lastSpawnRetry>1000 then
   a.lastSpawnRetry=now()
   if attach(a)then a.spawnRetry=nil;verifyPopulation()
   elseif a.startError~='No free safe spawn next to operator'then a.done=true;a.status='START_FAILED';emit(a,'START_FAILED',{error=a.startError})end
  end
  return
 end
 local p=a.id and Player(a.id)
 if p and not p:isRemoved()and cfg.parkedPlayers and cfg.parkedPlayers[a.name]then a.status='PARKED_TEMPLE';return end
 if not p or p:isRemoved()then
  local previousId=a.id;if a.id then byId[a.id]=nil end;a.id=nil
  if a.deadAt then
   if now()-a.deadAt>5000 and(not a.lastRetry or now()-a.lastRetry>5000)then a.lastRetry=now();if not attach(a)then emit(a,'RESPAWN_WAIT',{error=a.startError})end end
  else
   emit(a,'UNEXPECTED_PLAYER_REMOVAL',{action='RETRY_NATIVE_SAVED_CHARACTER',deathConfirmed=false})
   a.status='REMOVAL_RETRY';a.deadAt=now();a.done=false
   if a.brain then
    a.brain.stats.unknownRemovals=(a.brain.stats.unknownRemovals or 0)+1
    local class=getmetatable(a.brain);for key,lease in pairs(class and class.claims or{})do if lease.owner==previousId then class.claims[key]=nil end end
    if a.brain.policy.pending then
     local result=a.brain.policy:finish('UNCERTAIN','REMOVED',{},true)
     emit(a,'META_CENSORED',{reason='UNEXPECTED_REMOVAL',reward=result.reward});a.brain.goal=nil
    end
    local ok,why=pcall(function()a.brain:save(nil,false)end)
    if not ok then emit(a,'SAVE_ERROR',{error=tostring(why)})end
   end
  end
  return
 end
 p:cromeHeartbeat()
 local labInventory=((a.variant=='candidate')or(a.variant=='tibinha')or(cfg.armByName~=nil))and candidateInventory or inventory
 LabFood.ensure(p,labInventory,function(event,data)emit(a,event,data)end)
 for _,c in ipairs(Game.getSpectators(p:getPosition(),false,false,7,7,5,5))do if c:isMonster()then c:registerEvent('CromeABOutgoing')end end
 if a.deadAt then return end
 if hasLevelGoal and p:getLevel()>=cfg.goalLevel then
  if not a.brain.stats.finished or a.brain.stats.finishedGoal~=cfg.goalLevel then a.brain.stats.finished=raceElapsed();a.brain.stats.finishedGoal=cfg.goalLevel end
  if a.brain.policy.pending then a.brain.policy:reward('LEVEL_GOAL',10)end
  stop(a,'GOAL_LEVEL'..cfg.goalLevel);return
 end
 a.lastThink=now();a.brain.fastNeedsThink=false
 a.brain.router.nav.searchBudgetMs=8;a.brain.router.nav.searchSpent=0;a.brain.router.nav.searchPreviewSpent=0
 local started=now();local ok,why=xpcall(function()a.brain:tick(p)end,debug.traceback)
 local tickMillis=now()-started;a.tickMax=math.max(a.tickMax or 0,tickMillis);a.tickSum=(a.tickSum or 0)+tickMillis;a.tickCount=(a.tickCount or 0)+1
 if now()-started>150 then emit(a,'SLOW_BRAIN_TICK',{millis=now()-started,goal=a.brain.goal and a.brain.goal.kind or 'NONE'})end
 if not ok then emit(a,'BRAIN_ERROR',{error=tostring(why)});stop(a,'SCRIPT_ERROR');return end
 local pos=p:getPosition();local b=cfg.bounds
 if pos.x<b.minX or pos.x>b.maxX or pos.y<b.minY or pos.y>b.maxY or pos.z<b.minZ or pos.z>b.maxZ then emit(a,'OUTSIDE_AUDITED_REGION',{position=snapshot(a).position});stop(a,'OUTSIDE_REGION');return end
 if now()-a.lastSave>60000 then
  a.lastSave=now();local saveStarted=now();local ok,why=pcall(function()a.brain:save(p,false);p:save()end);if now()-saveStarted>50 then emit(a,'SLOW_SAVE',{millis=now()-saveStarted})end;if not ok then emit(a,'SAVE_ERROR',{error=tostring(why)})end
 end
end
local function loop()
 if shutting then return end
 for _,a in ipairs(actors)do if not a.done then local p=a.id and Player(a.id);if p then p:cromeHeartbeat()end end end
 if eventLog and now()-lastLogFlush>=500 then eventLog:flush();lastLogFlush=now()end
 local currentLoop=now();if lastLoop and currentLoop-lastLoop>150 then emit(nil,'CONTROLLER_STALL',{millis=currentLoop-lastLoop})end;if lastLoop and currentLoop-lastLoop>1000 then emit(nil,'CONTROLLER_LAG',{millis=currentLoop-lastLoop})end;lastLoop=currentLoop
 local flag=io.open('data/crome_race/STOP.flag','r');if flag then flag:close();os.remove('data/crome_race/STOP.flag');shutdown('OPERATOR_STOP');return end
 if cfg.seconds>0 and raceStart and now()-raceStart>=cfg.seconds*1000 then shutdown('TIME_LIMIT');return end
 if now()-lastMotion>=100 then
  lastMotion=now()
  for _,a in ipairs(actors)do
   local p=not a.done and a.id and Player(a.id)
   if p and not p:isRemoved()and not a.deadAt and a.brain then
    local ok,why=xpcall(function()a.brain:motion(p)end,debug.traceback)
    if not ok then emit(a,'MOTION_ERROR',{error=tostring(why)});a.brain.fastNeedsThink=true end
   end
  end
 end
 local routeStarted=now();local pendingRoutes={}
 for _,a in ipairs(actors)do if not a.done and a.brain and a.brain.router.searching then pendingRoutes[#pendingRoutes+1]=a end end
 table.sort(pendingRoutes,function(a,b)return(a.lastRouteWork or 0)<(b.lastRouteWork or 0)end)
 for _,a in ipairs(pendingRoutes)do
  if now()-routeStarted>=6 then break end
  local p=a.id and Player(a.id)
  if p and not p:isRemoved()then
   a.lastRouteWork=now();local ok,why=xpcall(function()a.brain.router:advanceSearch(p)end,debug.traceback)
   if not ok then emit(a,'ROUTE_CONTINUATION_ERROR',{error=tostring(why)});a.brain.router.searching=nil;a.brain.fastNeedsThink=true end
  end
 end
 local cycleStarted=now();local urgent=nil
 for _,a in ipairs(actors)do if not a.done and a.brain and a.brain.fastNeedsThink and now()-(a.lastThink or 0)>=150 then
  if not urgent or(a.lastThink or 0)<(urgent.lastThink or 0)then urgent=a end
 end end
 if urgent then step(urgent)end
 bucket=(bucket+1)%buckets
 verifyPopulation()
 local any=false
 local due={}
 for _,a in ipairs(actors)do if not a.done then any=true;if a.index%buckets==bucket and now()-(a.lastThink or 0)>=100 then due[#due+1]=a end end end
 table.sort(due,function(a,b)return(a.lastThink or 0)<(b.lastThink or 0)end)
 for _,a in ipairs(due)do if now()-cycleStarted>=25 then break end;step(a)end
 if not any then shutdown('ALL_ACTORS_FINISHED');return end
 if now()-lastBoard>5000 then lastBoard=now();board(false)end
 addEvent(loop,50)
end
local startup=GlobalEvent('CromeRace20Startup')
function startup.onStartup()
 os.remove('data/crome_race/STOP.flag')
 local rf=io.open('data/crome_race/RESET_LEVEL1.flag','r')
 local shouldReset=(cfg.resetLevel1AtBoot==true)
 if rf then rf:close();os.remove('data/crome_race/RESET_LEVEL1.flag');shouldReset=true end
 addEvent(function()
  local ok,why=xpcall(function()
   if shouldReset then resetAllPlayersToLevel1()end
   assert(Game.setCromeBotLimit,'Native 100-player adapter is missing')
   assert(MonsterType('Rat'):isIgnoringSpawnBlock()and MonsterType('Cave Rat'):isIgnoringSpawnBlock(),'Rat visibility-block flag missing')
   emit(nil,'RAT_RESPAWN_RULES_VALIDATED',{intervalSeconds=10,playerOnScreenAllowed=true,occupiedSpawnSquareAllowed=false})
   assert(Game.getExperienceForLevel(2)==100 and Game.getExperienceForLevel(3)==200 and Game.getExperienceForLevel(4)==400 and Game.getExperienceForLevel(8)==4200,'Wrong native experience curve')
   emit(nil,'NATIVE_XP_CURVE_VALIDATED',{level2=100,level3=200,level4=400,level8=4200})
   local invalid=Game.setCromeBotLimit(101);assert(not invalid,'Native upper bound must reject 101')
   assert(Game.setCromeBotLimit(cfg.players),'Could not set internal player limit')
   emit(nil,'LIMIT_RANGE_VALIDATED',{requested=cfg.players,max=100})
   emit(nil,'CONFIG',{players=cfg.players,controllerVersion='lab100-foodinf-regen2-motion',motionMode='CACHED_PATH_NATIVE_COOLDOWN',experimentCondition=cfg.experimentCondition,experimentRound=cfg.experimentRound,controllerBuild=cfg.controllerBuild,goalLevel=hasLevelGoal and cfg.goalLevel or 0,noLevelGoal=not hasLevelGoal,resetLevel1=shouldReset,checkpointLevel=cfg.checkpointLevel,seconds=cfg.seconds,experienceRules='ORIGINAL',items=7853,monsters=152,startingPoint=cfg.temple})
   for i=1,cfg.players do
    if not(cfg.offlinePlayers and cfg.offlinePlayers[cfg.names[i]])then
    local a={index=i,name=cfg.names[i],variant=cfg.groups[i]};actors[#actors+1]=a
    local completed=completedOfflineProfile(a)
    if completed then a.final=completed;a.status=completed.status;a.done=true;expectedOnline=expectedOnline-1;emit(a,'ALREADY_AT_LEVEL_GOAL',{level=completed.level,goal=cfg.goalLevel})
    elseif not attach(a)then
     if a.startError=='No free safe spawn next to operator'then a.spawnRetry=true;a.status='SPAWN_WAIT';a.done=false;emit(a,'SPAWN_WAIT',{reason=a.startError})
     else a.done=true;a.status='START_FAILED';emit(a,'START_FAILED',{error=a.startError})end
    end
   end
   end
   local threatEntries=0
   for _,bucket in pairs(CromeThreatIndex or{})do for _,spawn in ipairs(bucket)do assert(MonsterType(spawn.name):isHostile(),'Neutral animal in danger index');threatEntries=threatEntries+1 end end
   emit(nil,'HOSTILE_DANGER_INDEX_VALIDATED',{entries=threatEntries,neutralAnimalsExcluded=true})
   local count=0;for _,a in ipairs(actors)do if not a.done then count=count+1 end end
   emit(nil,'CONTROLLERS_SCHEDULED',{count=count,requested=expectedOnline})
   if count<expectedOnline then emit(nil,'SPAWN_PENDING',{online=count,requested=expectedOnline})end
   verifyPopulation()
   for _,a in ipairs(actors)do local p=a.id and Player(a.id);if p then p:cromeHeartbeat()end end
   raceStart=now();running=true;loop()
  end,debug.traceback)
  if not ok then emit(nil,'STARTUP_ERROR',{error=tostring(why)});shutdown('STARTUP_ERROR')end
 end,3000)
 return true
end
startup:register()
