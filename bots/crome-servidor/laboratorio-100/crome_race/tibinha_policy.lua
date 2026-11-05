-- Tibinha Bridge Policy: drop-in semi-Markov Q-learning policy with non-blocking file IPC for Tibinha (Pestinha).
-- Rubbing Tibinha (CARINHO) rewards the active meta (+Q); clicking Tibinha (POKE_WRONG) penalizes (-Q) and aborts it.
local BasePolicy=dofile('data/crome_race/policy.lua')
local TibinhaPolicy={};TibinhaPolicy.__index=TibinhaPolicy
setmetatable(TibinhaPolicy,{__index=BasePolicy})

local BRIDGE_DIR='data/crome_race/tibinha_bridge'
local function clean(s)return type(s)=='string'and not s:find('[\t\r\n]')end
local function slug(name)return(tostring(name or'Crome_001'):gsub('[^%w_]','_')):sub(1,60)end
local function altSlugFor(name)
 local s=tostring(name or'')
 local n=s:match('^Crome Novo (%d+)$')
 if n then return'Crome_'..n end
 n=s:match('^Crome (%d+)$')
 if n then return'Crome_Novo_'..n end
 return nil
end
local function quote(s)return'"'..tostring(s):gsub('\\','\\\\'):gsub('"','\\"'):gsub('\n','\\n'):gsub('\r','\\r'):gsub('\t','\\t')..'"'end
local function encodeJson(v)
 if v==nil then return'null'end
 if type(v)=='boolean'or type(v)=='number'then return tostring(v)end
 if type(v)=='string'then return quote(v)end
 if type(v)~='table'then return quote(tostring(v))end
 local n=#v;if n>0 then local out={};for i=1,n do out[i]=encodeJson(v[i])end;return'['..table.concat(out,',')..']'end
 local keys={};for k in pairs(v)do keys[#keys+1]=k end;table.sort(keys)
 local out={};for _,k in ipairs(keys)do out[#out+1]=quote(tostring(k))..':'..encodeJson(v[k])end
 return'{'..table.concat(out,',')..'}'
end

function TibinhaPolicy.new(o)
 local self=BasePolicy.new(o)
 setmetatable(self,TibinhaPolicy)
 self.actorName=o.actorName or'Crome 001'
 self.actorSlug=slug(self.actorName)
 self.altSlug=altSlugFor(self.actorName)
 self.bridgeDir=o.bridgeDir or BRIDGE_DIR
 self.emit=o.emit
 self.snapshotFn=o.snapshotFn
 self.tibinhaEnabled=o.tibinhaEnabled or false
 self.abortRequested=false
 self.lastBridgeCheck=0
 self.lastQSync=0
 return self
end

function TibinhaPolicy:openBridgeFile(suffix,mode)
 local f=io.open(self.bridgeDir..'/'..self.actorSlug..suffix,mode)
 if f then return f,self.bridgeDir..'/'..self.actorSlug..suffix end
 if self.altSlug then
  local f2=io.open(self.bridgeDir..'/'..self.altSlug..suffix,mode)
  if f2 then return f2,self.bridgeDir..'/'..self.altSlug..suffix end
 end
 return nil,nil
end

function TibinhaPolicy:bridgeActive()
 if self.tibinhaEnabled then return true end
 local t=self.clock()
 if t-self.lastBridgeCheck<2000 then return false end
 self.lastBridgeCheck=t
 local qf=self:openBridgeFile('_q.tsv','r')
 if qf then qf:close();self.tibinhaEnabled=true;return true end
 return false
end

function TibinhaPolicy:syncFromTibinhaQ()
 local t=self.clock()
 if t-self.lastQSync<500 then return end
 self.lastQSync=t
 local f=self:openBridgeFile('_q.tsv','r')
 if not f then return end
 local header=f:read('*l')
 if not header or not header:match('^CROME_POLICY_1\t')then f:close();return end
 for line in f:lines()do
  local u=line:match('^U\t(%d+)$')
  if u then self.updates=math.max(self.updates,tonumber(u)or 0)
  else
   local s,a,v,c=line:match('^Q\t([^\t]+)\t([^\t]+)\t([^\t]+)\t(%d+)$')
   local num=tonumber(v)
   if s and a and num and num==num and math.abs(num)<=1e6 then
    self.q[s]=self.q[s]or{};self.counts[s]=self.counts[s]or{}
    self.q[s][a]=num;self.counts[s][a]=tonumber(c)or 1
   end
  end
 end
 f:close()
end

function TibinhaPolicy:pollFeedback()
 if not self:bridgeActive()then return false end
 local slugs={self.actorSlug}
 if self.altSlug then slugs[#slugs+1]=self.altSlug end
 for _,sl in ipairs(slugs)do
  local fbPath=self.bridgeDir..'/'..sl..'_feedback.tsv'
  local f=io.open(fbPath,'r')
  if f then
   local lines={}
   for line in f:lines()do if #line>0 then lines[#lines+1]=line end end
   f:close()
   if #lines>0 then
    if self.atomic then pcall(self.atomic,fbPath,'')end
    for _,line in ipairs(lines)do
     local _ts,kind,valStr,metaId=line:match('^(%d+)\t([^\t]+)\t([^\t]+)\t([^\t]*)$')
     local val=tonumber(valStr)
     if kind and val and val==val and math.abs(val)<=100 then
      if self.pending then
       self:reward('TIBINHA_'..kind,val)
       if val<0 or kind=='POKE_WRONG'or kind=='DISPENSA'then
        self.abortRequested=true
       end
      end
      if self.emit then
       self.emit('TIBINHA_HUMAN_REWARD',{kind=kind,value=val,meta=metaId,abort=self.abortRequested,pendingMeta=self.pending and self.pending.action or'NONE'})
      end
     end
    end
   end
  end
 end
 return self.abortRequested
end

function TibinhaPolicy:exportState(state,allowed)
 if not self.atomic then return end
 local cand={}
 for i=1,math.min(10,#allowed)do
  local a=allowed[i]
  cand[#cand+1]={id=a.id,kind=a.kind or a.id:match('^([^:]+)')or'META',score=a.score or 0,routeStatus=a.routeStatus or'OK',goal=a.goal}
 end
 local payload={actor=self.actorName,domain=self.domain,ms=self.clock(),state=state,allowed=cand}
 if self.snapshotFn then
  local ok,snap=pcall(self.snapshotFn)
  if ok and type(snap)=='table'then
   for _,k in ipairs({'level','xp','hp','maxHp','money','position','goal'})do payload[k]=snap[k]end
  end
 end
 local encoded=encodeJson(payload)..'\n'
 pcall(self.atomic,self.bridgeDir..'/'..self.actorSlug..'_state.json',encoded)
 if self.altSlug then
  pcall(self.atomic,self.bridgeDir..'/'..self.altSlug..'_state.json',encoded)
 end
end

function TibinhaPolicy:readDecision(state,allowed)
 local f=self:openBridgeFile('_decision.json','r')
 if not f then return nil end
 local raw=f:read('*a');f:close()
 if not raw or #raw==0 then return nil end
 local action=raw:match('"action"%s*:%s*"([^"]+)"')
 local decState=raw:match('"state"%s*:%s*"([^"]+)"')
 local expStr=raw:match('"exploratory"%s*:%s*(%a+)')
 if not action or decState~=state then return nil end
 for _,a in ipairs(allowed)do
  if a.id==action then return a,(expStr=='true')end
 end
 return nil
end

function TibinhaPolicy:choose(state,allowed)
 if not self:bridgeActive()then return BasePolicy.choose(self,state,allowed)end
 assert(not self.pending,'Option result not finalized');assert(clean(state)and#allowed>0)
 self.abortRequested=false
 self:syncFromTibinhaQ()
 self:pollFeedback()
 self:exportState(state,allowed)

 local chosen,exploratory=self:readDecision(state,allowed)
 local fromDecision=(chosen~=nil)
 if not chosen then
  local best=nil;local score=nil
  for _,a in ipairs(allowed)do assert(clean(a.id));local v=self:value(state,a.id)+self.priorWeight*(a.score or 0)
   if not best or v>score or(v==score and(a.score or 0)>(best.score or 0))then best=a;score=v end
  end
  exploratory=self:random()<self.epsilon
  if exploratory then best=allowed[1+math.floor(self:random()*#allowed)]end
  chosen=best
 end
 self.pending={state=state,action=chosen.id,at=self.clock(),reward=0,terms={},confirmed=0,novel=0,fromTibinha=fromDecision}
 local qVal=self:value(state,chosen.id)
 if self.emit then
  self.emit('TIBINHA_META_CHOSEN',{id=chosen.id,state=state,q=qVal,exploratory=exploratory,directDecision=fromDecision})
 end
 return chosen,exploratory,qVal
end

function TibinhaPolicy:finish(status,nextState,allowed,terminal)
 if self:bridgeActive()then self:pollFeedback()end
 self.abortRequested=false
 local res=BasePolicy.finish(self,status,nextState,allowed,terminal)
 if self:bridgeActive()then
  local nextIds={}
  for i=1,math.min(8,#(allowed or{}))do nextIds[#nextIds+1]={id=allowed[i].id,score=allowed[i].score or 0}end
  local line=encodeJson({
   domain=self.domain,actor=self.actorName,state=res.state,action=res.action,
   status=status,reward=res.reward,old=res.old,value=res.value,
   duration=res.duration,confirmed=res.confirmed,updates=res.updates,
   nextState=nextState,allowed=nextIds,terminal=terminal and true or false,at=os.time()
  })
  local outFile=io.open(self.bridgeDir..'/'..self.actorSlug..'_outcomes.jsonl','a')
  if outFile then outFile:write(line..'\n');outFile:close()end
  if self.altSlug then
   local altOut=io.open(self.bridgeDir..'/'..self.altSlug..'_outcomes.jsonl','a')
   if altOut then altOut:write(line..'\n');altOut:close()end
  end
 end
 return res
end

function TibinhaPolicy:save()
 local ok,err=BasePolicy.save(self)
 if ok and self:bridgeActive()and self.path and self.atomic then
  local f=io.open(self.path,'r')
  if f then
   local content=f:read('*a');f:close()
   pcall(self.atomic,self.bridgeDir..'/'..self.actorSlug..'_q.tsv',content)
   if self.altSlug then pcall(self.atomic,self.bridgeDir..'/'..self.altSlug..'_q.tsv',content)end
  end
 end
 return ok,err
end

return TibinhaPolicy
