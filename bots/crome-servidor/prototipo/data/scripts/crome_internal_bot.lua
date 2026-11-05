-- Crome internal player prototype. Exactly one bot; no OTC or network packets.
-- Engine movement/attacks/food/loot keep their normal cooldowns and limits.
if CromeInternalBot and CromeInternalBot.stop then CromeInternalBot.stop('SCRIPT_RELOAD') end
local B = {active=nil, generation=0, interval=250, radius=10}
CromeInternalBot = B
local dirs={{x=0,y=-1,d=0},{x=1,y=0,d=1},{x=0,y=1,d=2},{x=-1,y=0,d=3}}
local function now() return os.mtime() end
local function copy(p) return {x=p.x,y=p.y,z=p.z} end
local function key(p) return p.x..','..p.y..','..p.z end
local function distance(a,b) if a.z~=b.z then return 999 end return math.max(math.abs(a.x-b.x),math.abs(a.y-b.y)) end
local function emit(event,detail)
    local a=B.active
    local line=string.format('[CROME_INTERNAL] %s player=%s %s',event,a and a.name or '-',detail or '')
    print(line)
    local f=io.open('data/crome_internal_bot.log','a')
    if f then f:write(os.date('%Y-%m-%d %H:%M:%S')..' '..line..'\n');f:close() end
end
local function state(name,detail)
    if B.active.state~=name then B.active.state=name;emit('STATE_'..name,detail) end
end
local function stopCombat(p) p:setTarget(nil);p:setFollowCreature(nil) end
function B.stop(reason)
    local a=B.active
    if not a then return false end
    B.generation=B.generation+1
    if a.timer then stopEvent(a.timer) end
    local p=Player(a.id)
    if p and p:isCromeInternalBot() then stopCombat(p);p:save();Game.stopCromeBot(a.id) end
    emit('STOP',reason or 'OPERATOR');B.active=nil;return true
end
local function visible(p)
    local result={}
    for _,c in ipairs(Game.getSpectators(p:getPosition(),false,false,7,7,5,5)) do
        if c:isMonster() and c:getPosition().z==p:getPosition().z and c:getType():isHostile() then result[#result+1]=c end
    end
    return result
end
local function allowed(p,q)
    local a=B.active
    return distance(q,a.anchor)<=B.radius and not (a.blocked[key(q)] and a.blocked[key(q)]>now()) and p:cromeCanStep(Position(q.x,q.y,q.z))
end
local function step(p,q,d)
    local a=B.active
    if a.pending or not allowed(p,q) then return false end
    local from=copy(p:getPosition())
    if p:cromeStep(d) then a.pending={from=from,to=copy(q),at=now()};return true end
    return false
end
-- Bounded BFS through currently visible tiles only. We never inspect an unseen
-- destination or issue a long blind walk list.
local function route(p,goal,adjacent)
    local origin=copy(p:getPosition());local seen={[key(origin)]=true};local queue={{p=origin}};local head=1
    while head<=#queue and head<=200 do
        local node=queue[head];head=head+1
        if (adjacent and distance(node.p,goal)<=1) or (not adjacent and key(node.p)==key(goal)) then return node.first end
        for _,v in ipairs(dirs) do
            local q={x=node.p.x+v.x,y=node.p.y+v.y,z=node.p.z}
            if not seen[key(q)] and distance(q,origin)<=7 and math.abs(q.y-origin.y)<=5 and allowed(p,q) then
                seen[key(q)]=true;queue[#queue+1]={p=q,first=node.first or {p=q,d=v.d}}
            end
        end
    end
    return nil
end
local function feed(p)
    local a=B.active
    if now()-a.foodAt<2000 then return false end
    local condition=p:getCondition(CONDITION_REGENERATION,CONDITIONID_DEFAULT)
    if condition and condition:getTicks()>30000 then return false end
    for _,id in ipairs({2696,2666,2674,2677}) do
        local food=p:getItemById(id,true)
        if food then
            local before=p:getItemCount(id);a.foodAt=now()
            local accepted=p:cromeUseFood(food)
            local after=p:getItemCount(id)
            emit('FOOD',string.format('id=%d accepted=%s consumed=%d',id,tostring(accepted),math.max(0,before-after)))
            return after<before
        end
    end
    return false
end
local function loot(p)
    local a=B.active;local job=a.loot
    if not job then return false end
    if now()-job.at>8000 then a.loot=nil;emit('LOOT_TIMEOUT');return false end
    -- A fleeing Rat may move between the last observation and the killing hit.
    -- Search a bounded, visible neighborhood; owner still has to be this Player.
    local body=nil;local found=nil;local score=nil;local here=p:getPosition()
    for dx=-2,2 do for dy=-2,2 do
        local q={x=job.pos.x+dx,y=job.pos.y+dy,z=job.pos.z}
        if q.z==here.z and math.abs(q.x-here.x)<=7 and math.abs(q.y-here.y)<=5 then
            local tile=Tile(Position(q.x,q.y,q.z))
            for _,item in ipairs(tile and tile:getItems() or {}) do
                if item:getId()==2813 and item:isContainer() and item:getCorpseOwner()==a.id then
                    local value=distance(q,here)
                    for i=0,item:getSize()-1 do local id=item:getItem(i):getId();if id==2148 or id==2696 then value=value-100;break end end
                    if not score or value<score then body=item;found=q;score=value end
                end
            end
        end
    end end
    if not body then state('LOOT','waiting for owned corpse observation');return true end
    state('LOOT')
    if not job.confirmed then emit('LOOT_BODY_CONFIRMED','pos='..key(found));job.confirmed=true end
    job.pos=found
    if distance(here,job.pos)>1 then
        local nextStep=route(p,job.pos,true)
        if nextStep then return step(p,nextStep.p,nextStep.d) end
        a.loot=nil;emit('LOOT_NO_ROUTE');return false
    end
    local bag=p:getSlotItem(CONST_SLOT_BACKPACK)
    if not bag or not bag:isContainer() then B.stop('BACKPACK_MISSING');return true end
    for i=0,body:getSize()-1 do
        local item=body:getItem(i);local id=item:getId()
        if id==2148 or (id==2696 and p:getItemCount(2696)<2) then
            local before=p:getItemCount(id)
            local accepted,reason=p:cromeTakeLoot(item,bag)
            local gained=p:getItemCount(id)-before
            if gained>0 then a.lootCount=a.lootCount+gained;emit('LOOT_CONFIRMED','id='..id..' count='..gained)
            elseif accepted==false and reason=='COOLDOWN' then emit('LOOT_WAIT','action cooldown; retry after observation')
            elseif accepted==false then emit('LOOT_REJECTED','reason='..tostring(reason)..'; preserve contents');a.loot=nil end
            return true
        end
    end
    a.loot=nil;emit('LOOT_DONE');return true
end
local function escape(p,monsters)
    local a=B.active;local here=copy(p:getPosition());local choices={}
    local function threat(q)
        local nearest=999;local adjacent=0
        for _,c in ipairs(monsters) do local d=distance(q,c:getPosition());nearest=math.min(nearest,d);if d<=1 then adjacent=adjacent+1 end end
        return nearest,adjacent
    end
    local current,currentAdjacent=threat(here)
    for _,d in ipairs(dirs) do
        local q={x=here.x+d.x,y=here.y+d.y,z=here.z}
        local away,adj=threat(q)
        if allowed(p,q) and adj<=currentAdjacent and away>=current then
            choices[#choices+1]={p=q,d=d.d,score=away*100-adj*1000-(a.visits[key(q)] or 0)}
        end
    end
    table.sort(choices,function(x,y)return x.score>y.score end)
    if choices[1] then return step(p,choices[1].p,choices[1].d) end
    return false
end
local function explore(p)
    local a=B.active;local here=copy(p:getPosition());local choices={}
    for _,d in ipairs(dirs) do
        local q={x=here.x+d.x,y=here.y+d.y,z=here.z}
        if allowed(p,q) then choices[#choices+1]={p=q,d=d.d,score=(a.visits[key(q)] or 0)*10+distance(q,a.anchor)*.1} end
    end
    table.sort(choices,function(x,y) return x.score<y.score end)
    if choices[1] then return step(p,choices[1].p,choices[1].d) end
    return false
end
function B.tick(generation)
    if generation~=B.generation or not B.active then return end
    local a=B.active;local p=Player(a.id)
    if not p or p:isRemoved() or not p:isCromeInternalBot() then emit('PLAYER_REMOVED_OR_DEAD');B.active=nil;return end
    p:cromeHeartbeat()
    if p:getHealth()<=0 then B.stop('DEATH');return end
    if p:getLevel()>=8 then B.stop('LEVEL_8');return end
    if now()-a.started>600000 then B.stop('TEN_MINUTE_PROTOTYPE_LIMIT');return end
    local here=copy(p:getPosition())
    if here.z~=a.anchor.z or distance(here,a.anchor)>B.radius then B.stop('OUTSIDE_TEST_AREA');return end
    if now()-a.savedAt>=30000 then
        a.savedAt=now();emit('SNAPSHOT',string.format('hp=%d/%d xp=%d gain=%d pos=%s save=%s',p:getHealth(),p:getMaxHealth(),p:getExperience(),p:getExperience()-a.initialXp,key(here),tostring(p:save())))
    end
    if a.pending then
        if key(here)==key(a.pending.to) then a.pending=nil
        elseif key(here)~=key(a.pending.from) then B.stop('UNEXPECTED_DISPLACEMENT');return
        elseif now()-a.pending.at>3000 then a.blocked[key(a.pending.to)]=now()+10000;a.pending=nil;emit('STEP_UNCONFIRMED')
        else return end
    end
    if a.lastPosition~=key(here) then a.visits[key(here)]=(a.visits[key(here)] or 0)+1;a.lastPosition=key(here) end
    local monsters=visible(p);local nearby=0;local unsupported=false
    for _,c in ipairs(monsters) do
        if distance(here,c:getPosition())<=3 then nearby=nearby+1 end
        if c:getName():lower()~='rat' then unsupported=true end
    end
    feed(p)
    local wounded=p:getHealth()/p:getMaxHealth()<.5
    if unsupported or nearby>1 or wounded then
        stopCombat(p);a.target=nil;state('RETREAT',unsupported and 'unsupported monster' or 'group/HP')
        if #monsters>0 then escape(p,monsters) else state('RECOVER','quiet; food regeneration uses server rules') end
        return
    end
    if a.target then
        local target=nil
        for _,c in ipairs(monsters) do if c:getId()==a.target.id then target=c end end
        if not target then
            if p:getExperience()>a.target.xp then a.loot={pos=a.target.pos,at=now()};emit('XP_CONFIRMED','gain='..(p:getExperience()-a.target.xp)) end
            stopCombat(p);a.target=nil
        else
            local percent=math.ceil(target:getHealth()*100/target:getMaxHealth())
            if percent<a.target.hp then a.target.hp=percent;a.target.progress=now() end
            a.target.pos=copy(target:getPosition())
            if now()-a.target.progress>15000 then
                a.skipped[target:getId()]=now()+20000;stopCombat(p);a.target=nil;emit('TARGET_NO_PROGRESS')
            else
                state('HUNTING')
                if not p:getTarget() or p:getTarget():getId()~=target:getId() then p:setTarget(target) end
                if distance(here,target:getPosition())>1 then local nextStep=route(p,target:getPosition(),true);if nextStep then step(p,nextStep.p,nextStep.d) end end
                return
            end
        end
    end
    if a.loot and nearby==0 and loot(p) then return end
    for _,c in ipairs(monsters) do
        if c:getName():lower()=='rat' and (not a.skipped[c:getId()] or a.skipped[c:getId()]<now()) then
            local nextStep=route(p,c:getPosition(),true)
            if distance(here,c:getPosition())<=1 or nextStep then
                a.target={id=c:getId(),xp=p:getExperience(),pos=copy(c:getPosition()),hp=math.ceil(c:getHealth()*100/c:getMaxHealth()),progress=now()}
                state('HUNTING','target='..c:getId());p:setTarget(c)
                if nextStep then step(p,nextStep.p,nextStep.d) end
                return
            end
        end
    end
    state('EXPLORE');explore(p)
end
local function schedule(generation)
    if generation~=B.generation or not B.active then return end
    B.active.timer=addEvent(function()
        if generation~=B.generation or not B.active then return end
        local ok,error=xpcall(function()B.tick(generation)end,debug.traceback)
        if not ok then emit('ERROR',tostring(error));B.stop('SCRIPT_ERROR');return end
        schedule(generation)
    end,B.interval)
end
function B.start(operator,name)
    if B.active then return false,'An internal bot is already active' end
    if type(Game.startCromeBot)~='function' then return false,'Server executable does not contain the Crome patch' end
    local id,error=Game.startCromeBot(name,operator:getPosition())
    if not id then return false,error end
    local p=Player(id)
    if not p or not p:isCromeInternalBot() then return false,'Internal player did not remain online' end
    B.generation=B.generation+1
    B.active={id=id,name=p:getName(),anchor=copy(p:getPosition()),started=now(),savedAt=now(),foodAt=0,visits={},blocked={},skipped={},initialXp=p:getExperience(),lootCount=0}
    emit('START','real Player, local Rat test, engine cooldowns, no client')
    schedule(B.generation);return true,'Started '..p:getName()
end
local talk=TalkAction('/cromebot')
function talk.onSay(operator,words,param)
    if not operator:getGroup():getAccess() or operator:getAccountType()<ACCOUNT_TYPE_GAMEMASTER then return false end
    local command,name=param:match('^%s*(%w+)%s*,?%s*(.-)%s*$')
    command=(command or ''):lower()
    local ok,message
    if command=='start' and name~='' then ok,message=B.start(operator,name)
    elseif command=='stop' then ok=B.stop('OPERATOR');message=ok and 'Stopped and saved' or 'No active internal bot'
    elseif command=='status' then ok=true;message=B.active and (B.active.name..' state='..tostring(B.active.state)..' id='..B.active.id) or 'No active internal bot'
    else message='Use /cromebot start,CharacterName | /cromebot stop | /cromebot status' end
    operator:sendTextMessage(MESSAGE_STATUS_CONSOLE_BLUE,message or 'Action failed')
    return false
end
talk:separator(' ');talk:register()
