-- One normal NPC conversation at a time per vendor; results require item/money deltas.
local M={leases={}}
local function sayTo(p,vendor,text)
 local npc=Npc and Npc(vendor.name)or nil
 if npc then return p:say(text,TALKTYPE_SAY,false,npc)end
 return p:say(text,TALKTYPE_SAY)
end
function M.release(a,p)
 local j=a.trade;if not j then return end
 sayTo(p,j.vendor,'bye');if M.leases[j.vendor.name]and M.leases[j.vendor.name].id==a.id then M.leases[j.vendor.name]=nil end;a.trade=nil
end
function M.tick(a,p,goal,emit)
 if goal.kind=='HEAL'and p:getHealth()>=65 then if a.trade then M.release(a,p)end;return 'COMPLETED'end
 local now=os.mtime();local lease=M.leases[goal.vendor.name]
 if not a.trade then
  if goal.kind=='BUY'and(goal.price or 0)>p:getMoney()then emit('TRADE_PRECONDITION_REJECTED',{reason='INSUFFICIENT_MONEY',npc=goal.vendor.name});return 'UNCERTAIN'end
  if lease and lease.id~=a.id and lease.untilAt>now then return 'WAIT_NPC' end
  M.leases[goal.vendor.name]={id=a.id,untilAt=now+14000}
  a.trade={vendor=goal.vendor,at=now,stage='HI',item=goal.item,before=p:getItemCount(goal.item or 0),money=p:getMoney(),hp=p:getHealth()}
  sayTo(p,goal.vendor,'hi');emit('NPC_HI_SENT','npc='..goal.vendor.name);return 'PENDING'
 end
 local j=a.trade
 if now-j.at>12000 then emit('TRADE_TIMEOUT',{npc=goal.vendor.name,stage=j.stage,item=j.item,moneyBefore=j.money,moneyAfter=p:getMoney(),countBefore=j.before,countAfter=j.item and p:getItemCount(j.item)or 0});M.release(a,p);return 'UNCERTAIN' end
 if goal.kind=='HEAL'then
  if p:getHealth()>=65 then emit('NPC_HEAL_OBSERVED','hp='..p:getHealth());M.release(a,p);return 'COMPLETED' end
  if j.stage=='HI'and now-j.at>1500 then sayTo(p,goal.vendor,'heal');j.stage='HEAL';emit('NPC_HEAL_SENT','npc='..goal.vendor.name)end
  return 'PENDING'
 end
 local count=p:getItemCount(j.item);local money=p:getMoney()
 if(goal.kind=='BUY'and count>j.before and money<j.money)or(goal.kind=='SELL'and count<j.before and money>j.money)then
  emit('TRADE_CONFIRMED','kind='..goal.kind..' npc='..goal.vendor.name..' id='..j.item..' countDelta='..(count-j.before)..' moneyDelta='..(money-j.money));M.release(a,p);return 'COMPLETED',money-j.money
 end
 if j.stage=='HI'and now-j.at>=1500 then sayTo(p,goal.vendor,goal.kind:lower()..' '..goal.name);j.stage='OFFER';j.offerAt=now;emit('NPC_OFFER_SENT','kind='..goal.kind..' name='..goal.name)
 elseif j.stage=='OFFER'and now-j.offerAt>=1500 then sayTo(p,goal.vendor,'yes');j.stage='YES';emit('NPC_YES_SENT','npc='..goal.vendor.name)
 elseif j.stage=='YES'and now-j.offerAt>5000 and money~=j.money and count==j.before then emit('TRADE_MONEY_WITHOUT_EXPECTED_ITEM','moneyDelta='..(money-j.money));M.release(a,p);return 'UNCERTAIN' end
 return 'PENDING'
end
return M
