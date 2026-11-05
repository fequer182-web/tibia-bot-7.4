-- Persistent, observation-led navigation. Prior tiles are hints; live checks execute steps.
local Nav={};Nav.__index=Nav
local dirs={{0,-1,0},{1,0,1},{0,1,2},{-1,0,3}}
local diagonals={{0,-1,0},{1,0,1},{0,1,2},{-1,0,3},{-1,1,4,3},{1,1,5,3},{-1,-1,6,3},{1,-1,7,3}}
local function key(p)return p.x..','..p.y..','..p.z end
local function coord(k)local x,y,z=k:match('^(%d+),(%d+),(%d+)$');if not x then return end;return{x=tonumber(x),y=tonumber(y),z=tonumber(z)}end
local function dist(a,b)return math.abs(a.x-b.x)+math.abs(a.y-b.y)end
local function finite(n)return n and n==n and math.abs(n)<1e12 end
local function atomic(path,text)
 local f,err=io.open(path..'.tmp','w');if not f then return false,err end
 local ok=f:write(text);local closed=f:close();if not ok or not closed then return false,'WRITE_FAILED' end
 os.remove(path..'.bak');local existed=io.open(path,'r');if existed then existed:close();if not os.rename(path,path..'.bak')then return false,'BACKUP_FAILED' end end
 if not os.rename(path..'.tmp',path)then os.rename(path..'.bak',path);return false,'RENAME_FAILED' end
 return true
end
function Nav.new(o)
 assert(type(o.domain)=='string'and not o.domain:find('[\t\r\n]'))
 local n=setmetatable({domain=o.domain,path=o.path,diagonal=o.diagonal or false,prior=o.prior or{},clock=o.clock or os.mtime,tiles={},temporary={},failed={},visited={},pending=nil,sequence=0,confirmed=0,uncertain=0,loaded=0},Nav)
 if o.path then
  local f=io.open(o.path,'r')
  if f then
   assert(f:read('*l')=='CROME_NAV_1\t'..o.domain,'Navigation memory domain/schema mismatch')
   for line in f:lines()do
    local k,w,v=line:match('^T\t([%d,]+)\t([01])\t(%d+)$');assert(k,'Invalid navigation memory row')
    local p=coord(k);assert(p and p.x>0 and p.x<65535 and p.y>0 and p.y<65535 and p.z<16,'Invalid tile coordinates')
    n.tiles[k]=w=='1';n.visited[k]=tonumber(v);n.loaded=n.loaded+1
   end
   f:close()
  end
 end
 return n
end
function Nav:observe(p,walkable,reason)
 local k=key(p)
 if reason=='CREATURE'or reason=='HAZARD'or reason=='THREAT'then self.temporary[k]=self.clock()+2000;return end
 self.tiles[k]=not not walkable;self.temporary[k]=nil
end
function Nav:walkable(p)
 local k=key(p)
 if self.temporary[k]and self.temporary[k]>self.clock()then return false end
 if self.tiles[k]~=nil then return self.tiles[k]end
 return self.prior[k]==true
end
function Nav:visit(p)
 local k=key(p);local first=(self.visited[k]or 0)==0
 self.tiles[k]=true;self.visited[k]=(self.visited[k]or 0)+1
 return first
end
-- Accepted is only a pending transaction: no visit/reward is assigned here.
function Nav:sent(from,to)
 assert(not self.pending,'Movement transaction still pending')
 self.sequence=self.sequence+1;self.pending={id=self.sequence,from=key(from),to=key(to),at=self.clock()}
 return self.sequence
end
function Nav:observePosition(p)
 local j=self.pending;if not j then return nil end;local k=key(p)
 if k==j.to then self.pending=nil;self.confirmed=self.confirmed+1;return{status='COMPLETED',id=j.id,novel=self:visit(p),to=p}end
 if k~=j.from then self.pending=nil;self.uncertain=self.uncertain+1;return{status='UNCERTAIN',reason='UNEXPECTED_DISPLACEMENT',id=j.id}end
 if self.clock()-j.at>=3000 then
  self.pending=nil;self.uncertain=self.uncertain+1;self.failed[j.from..'>'..j.to]=self.clock()+3000
  return{status='UNCERTAIN',reason='STEP_TIMEOUT',id=j.id}
 end
 return{status='SENT',id=j.id}
end
function Nav:plan(from,goal,o)
 o=o or{};if from.z~=goal.z then return{status='DIFFERENT_FLOOR'}end
 local function reached(p)return o.adjacent and dist(p,goal)<=1 or(not o.adjacent and key(p)==key(goal))end
 if reached(from)then return{status='REACHED',length=0,expanded=0}end
 local function push(heap,node)
  heap[#heap+1]=node;local i=#heap
  while i>1 do local p=math.floor(i/2);if heap[p].f<=node.f then break end;heap[i]=heap[p];i=p end;heap[i]=node
 end
 local function pop(heap)
  local first=heap[1];local last=table.remove(heap);if #heap==0 then return first end
  local i=1
  while i*2<=#heap do local c=i*2;if c+1<=#heap and heap[c+1].f<heap[c].f then c=c+1 end;if heap[c].f>=last.f then break end;heap[i]=heap[c];i=c end
  heap[i]=last;return first
 end
 local open={{p=from,g=0,f=dist(from,goal),length=0}};local best={[key(from)]=0};local expanded=0;local limit=o.limit or 2000
 while #open>0 and expanded<limit do
  local n=pop(open);local nk=key(n.p)
  if n.g==best[nk]then
   expanded=expanded+1
   if reached(n.p)then local path={};local node=n;while node.prev do table.insert(path,1,{q=node.p,d=node.d});node=node.prev end;return{status='ROUTE',first=n.first,path=path,length=n.length,expanded=expanded}end
   for _,d in ipairs(self.diagonal and diagonals or dirs)do
    local q={x=n.p.x+d[1],y=n.p.y+d[2],z=n.p.z};local qk=key(q);local edge=self.failed[nk..'>'..qk]
    if math.abs(q.x-from.x)<=(o.radius or 40)and math.abs(q.y-from.y)<=(o.radius or 40)and self:walkable(q)and(not edge or edge<=self.clock())then
     local visits=o.returning and 0 or math.min(20,self.visited[qk]or 0)*.01
     local g=n.g+(d[4]or 1)+visits
     if not best[qk]or g<best[qk]then best[qk]=g;push(open,{p=q,g=g,f=g+dist(q,goal),first=n.first or{q=q,d=d[3]},length=n.length+1,prev=n,d=d[3]})end
    end
   end
  end
 end
 return{status=#open>0 and'BUDGET_EXHAUSTED'or'NO_ROUTE',expanded=expanded}
end
function Nav:frontiers(here,anchor,radius)
 local candidates={}
 for k,walk in pairs(self.tiles)do
  local p=coord(k)
  if walk and p.z==here.z and dist(p,here)>1 and math.abs(p.x-anchor.x)<=radius and math.abs(p.y-anchor.y)<=radius and self:walkable(p)then
   local unknown=0
   for _,d in ipairs(self.diagonal and diagonals or dirs)do local q={x=p.x+d[1],y=p.y+d[2],z=p.z};if self.tiles[key(q)]==nil then unknown=unknown+1 end end
   if unknown>0 then candidates[#candidates+1]={id='EXPLORE:'..k,kind='EXPLORE',goal=p,score=unknown*2-(self.visited[k]or 0)-dist(p,here)*.02}end
  end
 end
 table.sort(candidates,function(a,b)if a.score==b.score then return a.id<b.id end;return a.score>b.score end)
 local selected={};for i=1,math.min(10,#candidates)do selected[i]=candidates[i]end
 return selected
end
function Nav:save()
 if not self.path then return true end
 local keys={};for k in pairs(self.tiles)do keys[#keys+1]=k end;table.sort(keys)
 local lines={'CROME_NAV_1\t'..self.domain}
 for _,k in ipairs(keys)do local count=self.visited[k]or 0;assert(finite(count));lines[#lines+1]='T\t'..k..'\t'..(self.tiles[k]and'1'or'0')..'\t'..count end
 return atomic(self.path,table.concat(lines,'\n')..'\n')
end
Nav.key=key;Nav.atomic=atomic
return Nav
