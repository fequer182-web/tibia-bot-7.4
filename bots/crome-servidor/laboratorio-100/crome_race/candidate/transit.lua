-- Shared semantic transit graph, independent of either arm's hunting waypoints.
CromeTransitData=CromeTransitData or dofile('data/crome_race/transit_atlas.lua')
local data=CromeTransitData;local T={data=data,out={},reverse={},cache={},cacheCount=0}
local function key(q)return q.x..','..q.y..','..q.z end
for _,e in ipairs(data.edges)do
 e.key=key(e.access.pos)..'>'..e.to
 T.out[e.from]=T.out[e.from]or{};table.insert(T.out[e.from],e)
 T.reverse[e.to]=T.reverse[e.to]or{};table.insert(T.reverse[e.to],e)
end
function T.components(q)
 local exact=data.cells[key(q)];if exact then return{[exact]=true}end
 local out={};for _,d in ipairs({{1,0},{0,1},{-1,0},{0,-1}})do local n=data.cells[(q.x+d[1])..','..(q.y+d[2])..','..q.z];if n then out[n]=true end end;return out
end
function T.component(q)
 local exact=data.cells[key(q)];if exact then return exact end
 for n in pairs(T.components(q))do return n end
end
local function distances(goals,rejected)
 local keys={};for n in pairs(goals)do keys[#keys+1]=n end;table.sort(keys);local cacheKey=table.concat(keys,',');local filtered=false
 for _,untilAt in pairs(rejected or{})do if untilAt>os.mtime()then filtered=true;break end end
 if not filtered and T.cache[cacheKey]then return T.cache[cacheKey]end
 local d={};local q={};for _,n in ipairs(keys)do d[n]=0;q[#q+1]=n end
 local i=1
 while q[i]do local n=q[i];i=i+1
  for _,e in ipairs(T.reverse[n]or{})do
   if(not rejected or(rejected[e.key]or 0)<=os.mtime())and d[e.from]==nil then d[e.from]=d[n]+1;q[#q+1]=e.from end
  end
 end
 if not filtered then if T.cacheCount>=64 then T.cache={};T.cacheCount=0 end;T.cache[cacheKey]=d;T.cacheCount=T.cacheCount+1 end
 return d
end
function T.hops(from,to,rejected)
 local a=T.component(from);local goals=T.components(to)
 if not a or not next(goals)then return nil end
 return distances(goals,rejected)[a]or math.huge
end
function T.next(from,to,rejected)
 local a=T.component(from);local goals=T.components(to)
 if not a or not next(goals)then return nil,'UNMAPPED'end
 if goals[a]then return nil,'SAME_COMPONENT'end
 local d=distances(goals,rejected);if not d[a]then return nil,'NO_ROUTE'end
 local best=nil;local score=nil
 for _,e in ipairs(T.out[a]or{})do
  if d[e.to]and d[e.to]<d[a]and(rejected[e.key]or 0)<=os.mtime()then
   local q=e.access.pos;local cost=d[e.to]*1000+math.abs(q.x-from.x)+math.abs(q.y-from.y)+(e.certainty=='OBSERVED'and 0 or 8)
   if not score or cost<score then best=e;score=cost end
  end
 end
 return best,best and'TRANSIT'or'NO_ROUTE'
end
return T
