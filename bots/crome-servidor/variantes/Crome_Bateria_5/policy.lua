-- Tabular semi-Markov Q-learning over concrete, safety-masked navigation goals.
local Policy={};Policy.__index=Policy
local function clean(s)return type(s)=='string'and not s:find('[\t\r\n]')end
function Policy.new(o)
 local p=setmetatable({domain=o.domain,path=o.path,atomic=o.atomic,clock=o.clock or os.mtime,q={},counts={},updates=0,alpha=.2,gamma=.95,epsilon=.08,seed=o.seed or 17,pending=nil},Policy)
 if o.path then local f=io.open(o.path,'r');if f then
  assert(f:read('*l')=='CROME_POLICY_1\t'..o.domain,'Policy domain/schema mismatch')
  for line in f:lines()do
   local u=line:match('^U\t(%d+)$')
   if u then p.updates=tonumber(u)else
    local state,action,value,count=line:match('^Q\t([^\t]+)\t([^\t]+)\t([^\t]+)\t(%d+)$');assert(state,'Invalid policy row')
    value=tonumber(value);assert(value and value==value and math.abs(value)<=1e6,'Invalid Q value')
    p.q[state]=p.q[state]or{};p.counts[state]=p.counts[state]or{};p.q[state][action]=value;p.counts[state][action]=tonumber(count)
   end
  end;f:close()
 end end
 return p
end
function Policy:random()self.seed=(self.seed*48271)%2147483647;return self.seed/2147483647 end
function Policy:value(state,action)return(self.q[state]or{})[action]or 0 end
function Policy:choose(state,allowed)
 assert(not self.pending,'Option result not finalized');assert(clean(state)and#allowed>0)
 local best=nil;local score=nil
 for _,a in ipairs(allowed)do assert(clean(a.id));local v=self:value(state,a.id)
  if not best or v>score or(v==score and(a.score or 0)>(best.score or 0))then best=a;score=v end
 end
 local exploratory=self:random()<self.epsilon
 if exploratory then best=allowed[1+math.floor(self:random()*#allowed)]end
 self.pending={state=state,action=best.id,at=self.clock(),reward=0,terms={},confirmed=0,novel=0}
 return best,exploratory,self:value(state,best.id)
end
function Policy:reward(kind,value)
 assert(self.pending and type(value)=='number'and value==value and math.abs(value)<=1e6);local p=self.pending;p.reward=p.reward+value;p.terms[kind]=(p.terms[kind]or 0)+value
end
function Policy:confirmed(novel)
 assert(self.pending);self.pending.confirmed=self.pending.confirmed+1
 if novel then self.pending.novel=self.pending.novel+1;self:reward('NEW_CONFIRMED_TILE',.2)end
end
function Policy:finish(status,nextState,allowed,terminal)
 assert(self.pending and(status=='COMPLETED'or status=='UNCERTAIN'or status=='REJECTED'))
 local job=self.pending;self.pending=nil
 local duration=math.max(0,(self.clock()-job.at)/1000);job.reward=job.reward-.01*duration
 if status=='COMPLETED'then job.reward=job.reward+(job.novel>0 and .3 or -.05) elseif status=='REJECTED'then job.reward=job.reward-.2 else job.reward=job.reward-.1 end
 local future=0
 if not terminal then for _,a in ipairs(allowed or{})do future=math.max(future,self:value(nextState,a.id))end end
 -- If all valid actions have negative values, bootstrap their actual maximum.
 if not terminal and #(allowed or{})>0 then future=self:value(nextState,allowed[1].id);for i=2,#allowed do future=math.max(future,self:value(nextState,allowed[i].id))end end
 self.q[job.state]=self.q[job.state]or{};self.counts[job.state]=self.counts[job.state]or{}
 local old=self:value(job.state,job.action);local target=job.reward+(terminal and 0 or self.gamma^(duration/5)*future)
 self.q[job.state][job.action]=old+self.alpha*(target-old);self.counts[job.state][job.action]=(self.counts[job.state][job.action]or 0)+1;self.updates=self.updates+1
 return{state=job.state,action=job.action,status=status,reward=job.reward,old=old,value=self.q[job.state][job.action],confirmed=job.confirmed,duration=duration,updates=self.updates}
end
function Policy:save()
 assert(self.atomic,'Atomic writer missing');if not self.path then return true end
 local lines={'CROME_POLICY_1\t'..self.domain,'U\t'..self.updates};local states={};for s in pairs(self.q)do states[#states+1]=s end;table.sort(states)
 for _,s in ipairs(states)do local actions={};for a in pairs(self.q[s])do actions[#actions+1]=a end;table.sort(actions)
  for _,a in ipairs(actions)do lines[#lines+1]=table.concat({'Q',s,a,string.format('%.12g',self.q[s][a]),self.counts[s][a]},'\t')end
 end
 return self.atomic(self.path,table.concat(lines,'\n')..'\n')
end
return Policy
