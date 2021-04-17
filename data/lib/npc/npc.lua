-- add npc interaction lib
dofile('data/lib/npc/npc_utils.lua')
dofile('data/lib/npc/npc_interaction.lua')

-- Checks whether a message is being sent to npc
-- msgContains(message, keyword)
function msgContains(message, keyword)
	local message, keyword = message:lower(), keyword:lower()
	if message == keyword then
		return true
	end

	return message:find(keyword) and not message:find('(%w+)' .. keyword)
end

-- Npc talk
-- npc:talk({text, text2}) or npc:talk(text)
function Npc:talk(creature, text)
	if type(text) == "table" then
		for i = 0, #text do
			self:sendMessage(creature, text[i])
		end
	else
		self:sendMessage(creature, text)
	end
end

-- Npc send message to player
-- npc:sendMessage(text)
function Npc:sendMessage(creature, text)
	return self:say(text, TALKTYPE_PRIVATE_NP, true, creature)
end

function Npc:processOnSay(message, player, npcInteractions)
    if not player then
        return false
    end

    if not self:isInTalkRange(player:getPosition()) then
        return false
    end

    table.insert(npcInteractions, NpcInteraction:newDefaultByType(player, messageTypes.MESSAGE_GREET))
    table.insert(npcInteractions, NpcInteraction:newDefaultByType(player, messageTypes.MESSAGE_FAREWELL))

    for _, npcInteraction in pairs(npcInteractions) do
        local validInteraction = npcInteraction:getValidInteraction(self, player, message)
        if validInteraction then return validInteraction:execute(self, player) end
    end

    if self:isInteractingWithPlayer(player) then
        self:talk(player, NpcInteraction:newDefaultByType(player).message)
    end

    return true
end
