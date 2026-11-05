-- One normal NPC conversation at a time per vendor; results require item/money deltas.
local M={leases={}}
function M.release(a,p)
 local j=a.trade;if not j then return end
 p:say('bye',TALKTYPE_SAY);if M.leases[j.vendor.name]and M.leases[j.vendor.name].id==a.id then M.leases[j.vendor.name]=nil end;a.trade=nil
end
function M.tick(a,p,goal,emit)
 local now=os.mtime();local lease=M.leases[goal.vendor.name]
 if not a.trade then
  if lease and lease.id~=a.id and lease.untilAt>now then return 'WAIT_NPC' end
  M.leases[goal.vendor.name]={id=a.id,untilAt=now+14000}
  a.trade={vendor=goal.vendor,at=now,stage='HI',item=goal.item,before=p:getItemCount(goal.item or 0),money=p:getMoney(),hp=p:getHealth()}
  p:say('hi',TALKTYPE_SAY);emit('NPC_HI_SENT','npc='..goal.vendor.name);return 'PENDING'
 end
 local j=a.trade
 if now-j.at>12000 then M.release(a,p);return 'UNCERTAIN' end
 if goal.kind=='HEAL'then
  if p:getHealth()>=65 then emit('NPC_HEAL_OBSERVED','hp='..p:getHealth());M.release(a,p);return 'COMPLETED' end
  if j.stage=='HI'and now-j.at>1500 then p:say('heal',TALKTYPE_SAY);j.stage='HEAL';emit('NPC_HEAL_SENT','npc='..goal.vendor.name)end
  return 'PENDING'
 end
 local count=p:getItemCount(j.item);local money=p:getMoney()
 if(goal.kind=='BUY'and count>j.before and money<j.money)or(goal.kind=='SELL'and count<j.before and money>j.money)then
  emit('TRADE_CONFIRMED','kind='..goal.kind..' npc='..goal.vendor.name..' id='..j.item..' countDelta='..(count-j.before)..' moneyDelta='..(money-j.money));M.release(a,p);return 'COMPLETED',money-j.money
 end
 if j.stage=='HI'and now-j.at>=1500 then p:say(goal.kind:lower()..' '..goal.name,TALKTYPE_SAY);j.stage='OFFER';j.offerAt=now;emit('NPC_OFFER_SENT','kind='..goal.kind..' name='..goal.name)
 elseif j.stage=='OFFER'and now-j.offerAt>=1500 then p:say('yes',TALKTYPE_SAY);j.stage='YES';emit('NPC_YES_SENT','npc='..goal.vendor.name)
 elseif j.stage=='YES'and now-j.offerAt>5000 and money~=j.money and count==j.before then emit('TRADE_MONEY_WITHOUT_EXPECTED_ITEM','moneyDelta='..(money-j.money));M.release(a,p);return 'UNCERTAIN' end
 return 'PENDING'
end
return M
