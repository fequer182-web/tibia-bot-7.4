-- Explicit laboratory intervention, scoped to tagged internal Players and owned marked food.
local cfg=dofile('data/crome_race/config.lua')
local F={enabled=cfg.infiniteFood==true,multiplier=cfg.healthRegenMultiplier or 1}
function F.isReusable(player,item)
 return F.enabled and player:isCromeInternalBot()and item:getCustomAttribute('crome_lab_food')==1 and item:getCustomAttribute('crome_lab_owner')==player:getGuid()
end
function F.apply(player)
 if not F.enabled or not player:isCromeInternalBot()then return false end
 local condition=player:getCondition(CONDITION_REGENERATION,CONDITIONID_DEFAULT)
 if not condition then return false end
 local vocation=player:getVocation()
 condition:setParameter(CONDITION_PARAM_HEALTHGAIN,vocation:getHealthGainAmount()*F.multiplier)
 condition:setParameter(CONDITION_PARAM_HEALTHTICKS,vocation:getHealthGainTicks()*1000)
 return true
end
function F.ensure(player,inventory,emit)
 if not F.enabled or not player:isCromeInternalBot()then return end
 local found=nil
 for _,row in ipairs(inventory:scan(player))do if F.isReusable(player,row.ref)then found=row.ref;break end end
 if not found then
  -- addItem may merge into a stack and return a removed temporary item.
  -- Tag the actual owned apple found by a fresh inventory scan.
  local item=nil
  for _,row in ipairs(inventory:scan(player))do if row.id==2674 and not row.body then item=row.ref;break end end
  if not item then
   local added=player:addItem(2674,1,false)
   if added then for _,row in ipairs(inventory:scan(player))do if row.id==2674 and not row.body then item=row.ref;break end end end
  end
  if not item then emit('LAB_FOOD_GRANT_WAIT',{reason='NORMAL_INVENTORY_OR_CAPACITY_LIMIT'});F.apply(player);return false end
  item:setCustomAttribute('crome_lab_food',1)
  item:setCustomAttribute('crome_lab_owner',player:getGuid())
  item:setAttribute(ITEM_ATTRIBUTE_NAME,'alimento infinito de laboratorio')
  item:setAttribute(ITEM_ATTRIBUTE_DESCRIPTION,'Teste: alimento reutilizavel e regeneracao de vida x2. Exclusivo do dono no laboratorio.')
  assert(F.isReusable(player,item),'Owned lab food marker was not observed')
  emit('LAB_FOOD_GRANTED',{itemId=2674,owner=player:getGuid(),regenMultiplier=F.multiplier});found=item
 end
 F.apply(player)
 return found
end
return F
