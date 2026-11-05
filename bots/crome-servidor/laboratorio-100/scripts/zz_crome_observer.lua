local event=CreatureEvent('Crome100ObserverLogin')
function event.onLogin(player)
 if player:getName()=='Observador Crome'and player:getGroup():getAccess()then
  player:setGhostMode(true)
  player:sendTextMessage(MESSAGE_INFO_DESCR,'Laboratorio Crome: use /goto 1 (ou /goto Crome Novo 001 / /goto Crome 001). Observador invisivel.')
  local pid=player:getId()
  addEvent(function()
   local p=Player(pid)
   if not p then return end
   local t=Creature('Crome Novo 001')or Creature('Crome 001')
   if not t then
    for i=1,100 do
     t=Creature(string.format('Crome Novo %03d',i))or Creature(string.format('Crome %03d',i))
     if t then break end
    end
   end
   if t then
    p:teleportTo(t:getPosition())
    p:sendTextMessage(MESSAGE_STATUS_CONSOLE_BLUE,'Acompanhando '..t:getName()..' (Nv.'..t:getLevel()..'). Use /goto 1, /goto 2... para trocar.')
   end
  end,1500)
 end
 return true
end
event:register()
