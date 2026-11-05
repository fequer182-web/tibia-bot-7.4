local C={};C.__index=C
function C.new(brain)
 if not(brain.cfg.armByName and brain.cfg.armByName[brain.a.name]=='A_WAYPOINTS')then return end
 return setmetatable({brain=brain,data=dofile('data/crome_race/cave_circuits.lua'),index=1,cycle=1,blockedUntil={},failures=0},C)
end
function C:choose(p)
 local b=self.brain;local index=b.a.index;local names={'rat_1','rat_2','rat_west','rat_north'}
 if p:getLevel()>=3 and b.progression:ready(p)then names={'spider_bug_multifloor','mixed_west_equipped'}
 elseif p:getLevel()>=2 then local prof=b.progression:profile(p);if prof.armor>=6 and prof.weaponUtility>=8 then names[#names+1]='rat_caverat_multifloor'end end
 local first=math.floor((index-1)/2)%#names
 for offset=0,#names-1 do local n=names[(first+offset)%#names+1];if(self.blockedUntil[n]or 0)<=os.mtime()then return n end end
 return names[first+1]
end
function C:goal(p)
 local b=self.brain
 local name=self:choose(p)
 if self.name~=name then self.name=name;self.index=1;self.failures=0 end
 if(self.blockedUntil[name]or 0)>os.mtime()then return end
 local points=self.data[name];if not points or #points==0 then return end
 local here=p:getPosition();local point=points[self.index]
 -- Arrival is observed; a reference is never treated as an executed walk.
 for _=1,math.min(3,#points)do
  if not point.access and point.z==here.z and math.max(math.abs(point.x-here.x),math.abs(point.y-here.y))<=1 then self.index=self.index%#points+1;point=points[self.index]else break end
 end
 local id='CIRCUIT:'..name..':'..self.index
 local g={id=id,kind='CIRCUIT',goal={x=point.x,y=point.y,z=point.z},circuit=true,circuitName=name,circuitIndex=self.index,score=11}
 if point.access then
  if here.z==point.z then g.kind='ENTER';g.access=point.access
  else
   g.approachAccess=true;g.id=id..':approach'
   for _,d in ipairs({{1,0},{0,1},{-1,0},{0,-1},{1,1},{-1,-1}})do local q={x=point.x+d[1],y=point.y+d[2],z=point.z};if b.router.nav:walkable(q)then g.goal=q;break end end
  end
 end
 return g
end
function C:onResult(p,g,status,reason)
 if not g or not g.circuit then return end
 if status=='COMPLETED'then self.failures=0 else self.failures=self.failures+1 end
 self.brain.emit('CIRCUIT_POINT_RESULT',{cave=g.circuitName,index=g.circuitIndex,status=status,reason=reason,observed=p:getPosition(),source='OPERATOR_REFERENCE_DYNAMIC_PATHFINDING'})
 local points=self.data[g.circuitName]
 if self.name==g.circuitName and points and not(g.approachAccess and status=='COMPLETED')then self.index=g.circuitIndex%#points+1 end
 if self.failures>=3 then self.blockedUntil[g.circuitName]=os.mtime()+60000;self.failures=0;self.brain.emit('CIRCUIT_RECONSIDER',{cave=g.circuitName,seconds=60})end
end
return C
