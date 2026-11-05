-- Autonomous race: persistent actors, native combat/shops/movement and normal XP.
local cfg=dofile('data/crome_race/config.lua')
local wiki=dofile('data/crome_race/wiki.lua')
local prior=dofile('data/crome_race/map_prior.lua')
local Inventory=dofile('data/crome_race/inventory.lua')
local Brain=dofile('data/crome_race/brain.lua')
local Nav=dofile('data/crome_race/navigation.lua')
local inventory=Inventory.new(wiki)
local actors={};local byId={};local running=false;local shutting=false;local raceStart=nil;local bucket=0;local lastBoard=0
local function now()return os.mtime()end
local elapsedBefore=0
local clockFile=io.open('data/crome_race/elapsed.tsv','r')
if clockFile then local header=clockFile:read('*l');assert(header=='CROME_RACE_CLOCK_1\t'..cfg.domain,'Race clock domain mismatch');elapsedBefore=tonumber(clockFile:read('*l'))or 0;clockFile:close()end
local function raceElapsed()return elapsedBefore+(raceStart and now()-raceStart or 0)end
local function saveClock()assert(Nav.atomic('data/crome_race/elapsed.tsv','CROME_RACE_CLOCK_1\t'..cfg.domain..'\n'..math.floor(raceElapsed())..'\n'))end
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
local function emit(a,event,data)
 if type(data)~='table'then data={detail=tostring(data)}end
 local record={at=os.time(),ms=now(),actor=a and a.name or'RACE',event=event,data=data or{}}
 local line=json(record)
 local f=io.open('data/crome_race/events.jsonl','a');if f then f:write(line..'\n');f:close()end
 if event~='MOVE_COMPLETED'then print('[CROME_RACE] '..line)end
end
local function snapshot(a)
 local p=a.id and Player(a.id)
 if not p then return a.final or{name=a.name,status=a.status or'WAITING',index=a.index}end
 local q=p:getPosition();local s=a.brain and a.brain.stats or{}
 return{name=a.name,index=a.index,status=a.status or'ACTIVE',level=p:getLevel(),xp=p:getExperience(),hp=p:getHealth(),maxHp=p:getMaxHealth(),money=p:getMoney(),bank=p.getBankBalance and p:getBankBalance()or 0,position={x=q.x,y=q.y,z=q.z},goal=a.brain and a.brain.goal and a.brain.goal.id or'NONE',playedMillis=s.played or 0,deaths=s.deaths or 0,earned=s.earned or 0,spent=s.spent or 0,moves=s.moves or 0,transitions=s.transitions or 0,metas=s.metas or 0,uncertain=s.uncertain or 0,loops=s.loops or 0,xpPerMinute=(s.autonomyXp or 0)*60000/math.max(1,s.autonomyMillis or 0),idlePercent=(s.autonomyIdle or 0)*100/math.max(1,s.autonomyMillis or 0),goalsFiltered=a.brain and a.brain.selectionStats.filtered or 0,regionValue=a.brain and a.brain.memory:value(q)or 0,checkpoint3=s.checkpoint3,finishedMillis=s.finished}
end
local function board(final)
 local rows={};local active=0
 for _,a in ipairs(actors)do local row=snapshot(a);rows[#rows+1]=row;if not a.done then active=active+1 end end
 table.sort(rows,function(a,b)
  if a.finishedMillis or b.finishedMillis then if not a.finishedMillis then return false elseif not b.finishedMillis then return true else return a.finishedMillis<b.finishedMillis end end
  if(a.level or 0)~=(b.level or 0)then return(a.level or 0)>(b.level or 0)end
  if(a.xp or 0)~=(b.xp or 0)then return(a.xp or 0)>(b.xp or 0)end
  if(a.deaths or 0)~=(b.deaths or 0)then return(a.deaths or 0)<(b.deaths or 0)end
  return a.index<b.index
 end)
 local data={schema=1,at=os.time(),active=active,controllerVersion='autonomy-2.1',controllerBuild=cfg.controllerBuild,goalLevel=cfg.goalLevel,checkpointLevel=cfg.checkpointLevel,elapsedOverall=raceElapsed(),elapsedThisSegment=raceStart and now()-raceStart or 0,secondsLimit=cfg.seconds,xpRules='ORIGINAL_UNCHANGED_LEVEL_FORMULA_X4',final=final or false,players=rows}
 saveClock()
 local ok,why=Nav.atomic('data/crome_race/leaderboard.json',json(data)..'\n');if not ok then emit(nil,'BOARD_WRITE_ERROR',{error=tostring(why)})end
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
local function attach(a)
 local id,why=Game.startCromeBot(a.name,Position(cfg.temple.x,cfg.temple.y,cfg.temple.z),true)
 if not id then a.startError=tostring(why);return false end
 a.id=id;byId[id]=a;local p=Player(id)
 p:registerEvent('CromeRace20Death');p:registerEvent('CromeRace20Advance')
 local ok,b=pcall(Brain.new,a,cfg,wiki,prior,inventory,function(e,d)emit(a,e,d)end)
 if not ok then emit(a,'BRAIN_START_ERROR',{error=tostring(b)});Game.stopCromeBot(id);byId[id]=nil;a.id=nil;return false end
 a.brain=b;a.status='ACTIVE';a.done=false;a.deadAt=nil;a.lastSave=now()+a.index*150
 emit(a,'START',{index=a.index,level=p:getLevel(),xp=p:getExperience(),hp=p:getHealth(),position=snapshot(a).position,continued=true})
 return true
end
local death=CreatureEvent('CromeRace20Death')
function death.onDeath(player,corpse,killer,mostDamageKiller,...)
 local a=byId[player:getId()];if not a then return true end
 a.deadAt=now();a.status='DEAD_WAITING';a.final=snapshot(a);a.final.status='DEAD_WAITING'
 if a.brain then
  a.brain.stats.deaths=a.brain.stats.deaths+1
  a.brain.memory:death(player:getPosition())
  if a.brain.policy.pending then a.brain.policy:reward('DEATH',-30)end
  pcall(function()a.brain:save(player,true)end)
 end
 emit(a,'DEATH_CONFIRMED',{killer=killer and killer:getName()or'UNKNOWN',position=a.final.position,xp=player:getExperience(),money=player:getMoney()})
 return true
end
death:register()
local advance=CreatureEvent('CromeRace20Advance')
function advance.onAdvance(player,skill,old,new)
 local a=byId[player:getId()];if not a or skill~=SKILL_LEVEL then return true end
 local elapsed=raceElapsed();saveClock()
 if new>=cfg.checkpointLevel and not a.brain.stats.checkpoint3 then a.brain.stats.checkpoint3=elapsed;emit(a,'CHECKPOINT_LEVEL3',{level=new,elapsedMillis=elapsed,xp=player:getExperience()})end
 if new>=cfg.goalLevel and not a.brain.stats.finished then a.brain.stats.finished=elapsed;emit(a,'FINISH_LEVEL4',{elapsedMillis=elapsed,xp=player:getExperience()})end
 if a.brain.policy.pending then a.brain.policy:reward('LEVEL_GAIN',(new-old)*5)end
 return true
end
advance:register()
local function shutdown(reason)
 if shutting then return end;shutting=true
 emit(nil,'STOP_REQUESTED',{reason=reason})
 for _,a in ipairs(actors)do stop(a,reason)end
 board(true);Game.setGameState(GAME_STATE_SHUTDOWN)
end
local function step(a)
 if a.done then return end
 local p=a.id and Player(a.id)
 if not p or p:isRemoved()then
  if a.id then byId[a.id]=nil end;a.id=nil
  if a.deadAt then
   if now()-a.deadAt>5000 and(not a.lastRetry or now()-a.lastRetry>5000)then a.lastRetry=now();if not attach(a)then emit(a,'RESPAWN_WAIT',{error=a.startError})end end
  else
   emit(a,'UNEXPECTED_PLAYER_REMOVAL',{});a.status='REMOVED';a.done=true
  end
  return
 end
 p:cromeHeartbeat()
 if a.deadAt then return end
 if p:getLevel()>=cfg.goalLevel then
  if not a.brain.stats.finished then a.brain.stats.finished=raceElapsed() end
  if a.brain.policy.pending then a.brain.policy:reward('LEVEL_GOAL',10)end
  stop(a,'GOAL_LEVEL4');return
 end
 local ok,why=xpcall(function()a.brain:tick(p)end,debug.traceback)
 if not ok then emit(a,'BRAIN_ERROR',{error=tostring(why)});stop(a,'SCRIPT_ERROR');return end
 local pos=p:getPosition();local b=cfg.bounds
 if pos.x<b.minX or pos.x>b.maxX or pos.y<b.minY or pos.y>b.maxY or pos.z<b.minZ or pos.z>b.maxZ then emit(a,'OUTSIDE_AUDITED_REGION',{position=snapshot(a).position});stop(a,'OUTSIDE_REGION');return end
 if now()-a.lastSave>15000 then
  a.lastSave=now();local ok,why=pcall(function()a.brain:save(p,false);p:save()end);if not ok then emit(a,'SAVE_ERROR',{error=tostring(why)})end
 end
end
local function loop()
 if shutting then return end
 local flag=io.open('data/crome_race/STOP.flag','r');if flag then flag:close();shutdown('OPERATOR_STOP');return end
 if raceStart and now()-raceStart>=cfg.seconds*1000 then shutdown('TIME_LIMIT');return end
 bucket=(bucket+1)%5
 local any=false
 for _,a in ipairs(actors)do if not a.done then any=true;if a.index%5==bucket then step(a)end end end
 if not any then shutdown('ALL_ACTORS_FINISHED');return end
 if now()-lastBoard>5000 then lastBoard=now();board(false)end
 addEvent(loop,50)
end
local startup=GlobalEvent('CromeRace20Startup')
function startup.onStartup()
 addEvent(function()
  local ok,why=xpcall(function()
   emit(nil,'CONFIG',{players=cfg.players,controllerVersion='autonomy-2.1',controllerBuild=cfg.controllerBuild,goalLevel=cfg.goalLevel,checkpointLevel=cfg.checkpointLevel,seconds=cfg.seconds,experienceRules='ORIGINAL',items=7853,monsters=152,startingPoint=cfg.temple})
   for i=1,cfg.players do
    local a={index=i,name='Aventureiro '..string.format('%02d',i)};actors[#actors+1]=a
    if not attach(a)then a.done=true;a.status='START_FAILED';emit(a,'START_FAILED',{error=a.startError})end
   end
   local count=0;for _,a in ipairs(actors)do if not a.done then count=count+1 end end
   emit(nil,'CONCURRENT_ACTORS',{count=count,requested=cfg.players})
   if count==cfg.players then
    local extra,reason=Game.startCromeBot('Crome Interno',Position(cfg.temple.x,cfg.temple.y,cfg.temple.z))
    if extra then emit(nil,'LIMIT21_FAILED',{});Game.stopCromeBot(extra)else emit(nil,'LIMIT21_REJECTED',{reason=tostring(reason)})end
   end
   raceStart=now();running=true;loop()
  end,debug.traceback)
  if not ok then emit(nil,'STARTUP_ERROR',{error=tostring(why)});shutdown('STARTUP_ERROR')end
 end,3000)
 return true
end
startup:register()
