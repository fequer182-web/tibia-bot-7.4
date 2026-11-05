-- Evidence memory: wall-clock retry deadlines survive process restarts.
local A={};A.__index=A
local function valid(s)return type(s)=='string'and not s:find('[\t\r\n]')end
local function number(s)local n=tonumber(s);assert(n and n==n and n>=0 and n<1e12,'Invalid autonomy number');return n end
function A.region(p)return math.floor(p.x/16)..','..math.floor(p.y/16)..','..p.z end
function A.new(o)
 local a=setmetatable({domain=o.domain,path=o.path,atomic=o.atomic,clock=o.clock or os.time,failures={},regions={},loaded=0},A)
 local f=o.path and io.open(o.path,'r')
 if f then
  assert(f:read('*l')=='CROME_AUTONOMY_1\t'..o.domain,'Autonomy domain mismatch')
  for line in f:lines()do
   local fields={};for v in (line..'\t'):gmatch('(.-)\t')do fields[#fields+1]=v end
   if fields[1]=='F'then
    assert(#fields==5 and valid(fields[2])and valid(fields[5]),'Invalid failure row')
    a.failures[fields[2]]={count=number(fields[3]),untilAt=number(fields[4]),reason=fields[5]}
   else
    assert(fields[1]=='R'and #fields==10 and fields[2]:match('^%d+,%d+,%d+$'),'Invalid region row')
    local r={};for i,k in ipairs({'ms','xp','damage','gold','deaths','visits','idle','novel'})do r[k]=number(fields[i+2])end
    a.regions[fields[2]]=r
   end
   a.loaded=a.loaded+1
  end;f:close()
 end
 for id,failure in pairs(a.failures)do if id=='RECOVER:Cipfried'then failure.untilAt=math.min(failure.untilAt,a.clock()+10)end end
 return a
end
function A:allowed(id)local f=self.failures[id];return not f or f.untilAt<=self.clock()end
function A:outcome(id,status,reason)
 assert(valid(id)and valid(reason))
 if status=='COMPLETED'then self.failures[id]=nil;return end
 if reason=='SAFETY_RECONSIDERATION'or reason=='SESSION_END'or reason=='POLICY_RECONSIDERATION'then return end
 local old=self.failures[id];local count=math.min(10,(old and old.count or 0)+1)
 local base=id=='RECOVER:Cipfried'and 3 or reason=='BUSH_NOT_READY'and 120 or(reason=='TARGET_DISAPPEARED'and 5 or 20)
 self.failures[id]={count=count,untilAt=self.clock()+math.min(id=='RECOVER:Cipfried'and 15 or 900,base*2^(count-1)),reason=reason}
end
function A:pause(id,seconds,reason)
 local old=self.failures[id];self.failures[id]={count=old and old.count or 0,untilAt=self.clock()+seconds,reason=reason}
end
function A:record(p,ms,xp,damage,gold,idle,novel)
 local k=A.region(p);local r=self.regions[k]
 if not r then r={ms=0,xp=0,damage=0,gold=0,deaths=0,visits=0,idle=0,novel=0};self.regions[k]=r end
 r.ms=r.ms+math.max(0,ms or 0);r.xp=r.xp+math.max(0,xp or 0);r.damage=r.damage+math.max(0,damage or 0);r.gold=r.gold+math.max(0,gold or 0)
 r.idle=r.idle+math.max(0,idle or 0);r.novel=r.novel+(novel and 1 or 0)
 return r
end
function A:arrive(p)local r=self:record(p);r.visits=r.visits+1 end
function A:death(p)local r=self:record(p);r.deaths=r.deaths+1 end
function A:value(p)
 local r=self.regions[A.region(p)];if not r then return 0 end
 local minutes=math.max(.5,r.ms/60000)
 return math.max(-12,math.min(6,(r.xp+r.gold*.1-r.damage*.25)/minutes*.08-r.deaths*6/math.max(1,r.visits)-math.min(4,r.idle/60000)))
end
function A:save()
 if not self.path then return true end
 local rows={'CROME_AUTONOMY_1\t'..self.domain};local keys={}
 for k in pairs(self.failures)do keys[#keys+1]=k end;table.sort(keys)
 for _,k in ipairs(keys)do local f=self.failures[k];rows[#rows+1]=table.concat({'F',k,f.count,f.untilAt,f.reason},'\t')end
 keys={};for k in pairs(self.regions)do keys[#keys+1]=k end;table.sort(keys)
 for _,k in ipairs(keys)do local r=self.regions[k];rows[#rows+1]=table.concat({'R',k,r.ms,r.xp,r.damage,r.gold,r.deaths,r.visits,r.idle,r.novel},'\t')end
 return self.atomic(self.path,table.concat(rows,'\n')..'\n')
end
return A
