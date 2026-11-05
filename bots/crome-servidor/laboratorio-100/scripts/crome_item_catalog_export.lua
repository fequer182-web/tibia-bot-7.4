local cromeItemExport = GlobalEvent("CromeItemCatalogExport")

local OUTPUT_FILE = "data/crome_item_catalog.tsv"
local MAX_SERVER_ID = 65535

local CHECK_CLIENT_IDS = {
    3270,
    3412,
    3355,
    3361,
    2853,
    3561,
    3031,
    3607
}

local weaponNames = {}

local function addWeapon(value, name)
    if type(value) == "number" then
        weaponNames[value] = name
    end
end

addWeapon(WEAPON_NONE, "none")
addWeapon(WEAPON_SWORD, "sword")
addWeapon(WEAPON_CLUB, "club")
addWeapon(WEAPON_AXE, "axe")
addWeapon(WEAPON_SHIELD, "shield")
addWeapon(WEAPON_DISTANCE, "distance")
addWeapon(WEAPON_WAND, "wand")
addWeapon(WEAPON_AMMO, "ammo")

local function safeCall(func, defaultValue)
    local ok, value = pcall(func)

    if ok and value ~= nil then
        return value
    end

    return defaultValue
end

local function cleanText(value)
    value = tostring(value or "")

    value = value:gsub("\t", " ")
    value = value:gsub("\r", " ")
    value = value:gsub("\n", " ")

    return value
end

local function getWeaponName(itemType)
    local weaponType = safeCall(function()
        return itemType:getWeaponType()
    end, 0)

    return weaponNames[weaponType] or tostring(weaponType)
end

local function getItemData(serverId)
    local ok, itemType = pcall(ItemType, serverId)

    if not ok or not itemType then
        return nil
    end

    local realServerId = safeCall(function()
        return itemType:getId()
    end, 0)

    if realServerId ~= serverId then
        return nil
    end

    local clientId = safeCall(function()
        return itemType:getClientId()
    end, 0)

    if clientId == 0 then
        return nil
    end

    local name = safeCall(function()
        return itemType:getName()
    end, "")

    if name == "" then
        return nil
    end

    return {
        serverId = serverId,
        clientId = clientId,

        name = cleanText(name),

        weaponType = getWeaponName(itemType),

        attack = safeCall(function()
            return itemType:getAttack()
        end, 0),

        defense = safeCall(function()
            return itemType:getDefense()
        end, 0),

        extraDefense = safeCall(function()
            return itemType:getExtraDefense()
        end, 0),

        armor = safeCall(function()
            return itemType:getArmor()
        end, 0),

        shootRange = safeCall(function()
            return itemType:getShootRange()
        end, 0),

        hitChance = safeCall(function()
            return itemType:getHitChance()
        end, 0),

        attackSpeed = safeCall(function()
            return itemType:getAttackSpeed()
        end, 0),

        weight = safeCall(function()
            return itemType:getWeight()
        end, 0),

        slotPosition = safeCall(function()
            return itemType:getSlotPosition()
        end, 0)
    }
end

local function row(data)
    return table.concat({
        tostring(data.serverId),
        tostring(data.clientId),
        cleanText(data.name),
        cleanText(data.weaponType),
        tostring(data.attack),
        tostring(data.defense),
        tostring(data.extraDefense),
        tostring(data.armor),
        tostring(data.shootRange),
        tostring(data.hitChance),
        tostring(data.attackSpeed),
        tostring(data.weight),
        tostring(data.slotPosition)
    }, "\t")
end

function cromeItemExport.onStartup()

    print("============================================================")
    print("CROME ITEM CATALOG EXPORT")
    print("============================================================")

    local file, errorMessage = io.open(
        OUTPUT_FILE,
        "w"
    )

    if not file then
        print(
            "[CROME] ERRO abrindo arquivo: "
            .. tostring(errorMessage)
        )

        return true
    end

    file:write(
        table.concat({
            "serverId",
            "clientId",
            "name",
            "weaponType",
            "attack",
            "defense",
            "extraDefense",
            "armor",
            "shootRange",
            "hitChance",
            "attackSpeed",
            "weight",
            "slotPosition"
        }, "\t")
    )

    file:write("\n")

    local total = 0

    local clientToItem = {}

    for serverId = 1, MAX_SERVER_ID do

        local data = getItemData(
            serverId
        )

        if data then

            total = total + 1

            clientToItem[
                data.clientId
            ] = data

            file:write(
                row(data)
            )

            file:write("\n")
        end
    end

    file:close()

    print(
        "[CROME] Catalogo exportado:",
        OUTPUT_FILE
    )

    print(
        "[CROME] Itens exportados:",
        total
    )

    print()
    print(
        "============================================================"
    )

    print(
        "CROME CLIENT ID CHECK"
    )

    print(
        "============================================================"
    )

    for _, clientId in ipairs(
        CHECK_CLIENT_IDS
    ) do

        local data = clientToItem[
            clientId
        ]

        if data then

            print(
                string.format(
                    "client=%d server=%d name=\"%s\" type=%s atk=%d def=%d extraDef=%d armor=%d range=%d",
                    data.clientId,
                    data.serverId,
                    data.name,
                    data.weaponType,
                    data.attack,
                    data.defense,
                    data.extraDefense,
                    data.armor,
                    data.shootRange
                )
            )

        else

            print(
                string.format(
                    "client=%d -> NAO ENCONTRADO",
                    clientId
                )
            )
        end
    end

    print(
        "============================================================"
    )

    return true
end

cromeItemExport:register()