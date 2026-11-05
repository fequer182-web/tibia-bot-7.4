# Crome / Rifbot — Livro de Engenharia Reversa e Mapa para o Nosso SDK

> Versão pesquisável preservada no repositório a partir do documento DOCX fornecido ao projeto.
> Trate offsets e RVAs conforme a classificação do próprio livro: confirmado, candidato, legado ou desconhecido.
> O profile específico de cada build continua sendo a fonte de verdade para dados confirmados.

---

<PARSED TEXT FOR PAGE: 1 / 88>
CROME / RIFBOT
Livro de Engenharia Reversa e Mapa para o Nosso SDK
Extração estática do payload fornecido • foco: Olders x64 • 24/09/2026
<PARSED TEXT FOR PAGE: 2 / 88>
Como ler este livro
Este documento organiza o que foi recuperado do Crome.exe em camadas. Ele não presume que todo número de um 
profile antigo vale no cliente atual. Cada dado é tratado como: confirmado no nosso build, candidato do profile atual 
do Crome, legado de build antiga, ou ainda desconhecido. O objetivo é permitir construir um SDK e um bot próprios 
sem misturar lógica de automação com offsets específicos de build.
Corpus recuperado: 68.030 linhas de representação AutoIt. 962 funções. 17.802 strings decodificadas e 7.289 globais 
numéricas.
1. Arquitetura geral
A arquitetura funcional observada pode ser reconstruída como: detecção do cliente/build profile camada de → →
memória objetos de jogo ações módulos de bot API/script. Essa separação é a parte mais importante para a → → → →
nossa implementação.
 Build/Profile: identifica executável e escolhe offsets/layouts.
 Memory: abre processo, resolve módulo/base, lê/escreve memória e fornece primitivas.
 SDK: Creature, LocalPlayer, Item, Tile, Map, Container e Inventory.
 Actions: attack/follow/walk/look/use/useWith/move/open/close.
 Automation: targeting, walker/cavebot, healing/conditions e looter.
 Scripting/API: expõe operações de alto nível sem obrigar script a conhecer offsets.
2. Olders - evolução dos layouts
O payload contém identificadores Olders distintos, incluindo olders_april, olders_september e olders_profiles, além 
de olders_opengl.exe e olders_dx9.exe. Os layouts mudam entre famílias; portanto o profile deve ser uma entidade de
primeira classe.
2.1 Profile atual que cruza com nosso build
Campo Offset Status
Creature.X +0x18 confirmed both
Creature.Y +0x1C confirmed both
Creature.Z +0x20 confirmed both
Creature.ID +0x38 confirmed both
Creature.Name +0x40 confirmed both
Creature.HP% +0x60 confirmed both
Creature.Direction +0x64 confirmed both
Creature.Alive/Removed-related +0x1A6 Crome current + our offline/runtime
context
Creature.Skull +0xFC Crome current candidate; runtime 
semantic validation pending
Creature.Party +0xFD Crome current candidate; runtime 
semantic validation pending
LocalPlayer.States +0x648 confirmed offset; Crome logic maps 
<PARSED TEXT FOR PAGE: 3 / 88>
key bits
LocalPlayer.Health +0x658 confirmed both
LocalPlayer.MaxHealth +0x660 confirmed both
LocalPlayer.Capacity +0x668 confirmed both
LocalPlayer.Experience +0x678 confirmed both
LocalPlayer.Level +0x680 confirmed both
LocalPlayer.Mana +0x690 confirmed both
LocalPlayer.MaxMana +0x698 confirmed both
LocalPlayer.MagicLevel +0x6A0 confirmed both
2.2 Exemplo de drift em profile Olders legado
Campo Offset legado
Creature.X +0x0C
Creature.Y +0x10
Creature.Z +0x14
Creature.ID +0x2C
Creature.Name +0x30
Creature.HP% +0x48
Creature.Direction +0x4C
Creature.Skull +0xD0
Creature.Party +0xD1
Creature.Attack +0x144
Creature.Alive +0x146
3. Creature
A estrutura Lua-facing recuperada do Rifbot é: id, name, x, y, z, hpperc, alive, direction, addr, attack, party, skull. Isso 
mostra quais propriedades o bot considera suficientes para snapshots e seleção de alvos.
No nosso SDK, Creature deve oferecer propriedades sem expor offsets ao restante do bot. O BuildProfile fornece os 
offsets; Creature apenas interpreta o objeto.
Campos atuais com forte cruzamento: posição +0x18/+0x1C/+0x20, ID +0x38, nome +0x40, HP% +0x60 e direção +0x64.
Skull +0xFC e Party +0xFD são candidatos fortes do profile atual do Crome, mas devem permanecer marcados como 
candidatos até validação semântica no nosso runtime.
4. LocalPlayer, stats e conditions
O offset de states do nosso LocalPlayer é +0x648. A lógica recuperada do Crome usa máscaras de estado para módulos
automáticos.
Mask Uso recuperado Interpretação
0x10 Mana Shield Utamo Vita / mana shield
0x20 AntiPara Paralyze
0x40 AutoHur Haste
0x80 usado por outra condição não nomear ainda
Regra para nossa cópia: Conditions.hpp define bits sem depender de profissão. O módulo Healing/Support consulta 
Player::states() e decide ações em camada superior.
<PARSED TEXT FOR PAGE: 4 / 88>
5. Map, Tile e mundo
O Crome expõe operações de mapa e o nosso laboratório já possui primitivas Map/Tile. A nossa camada deve 
representar Position de forma ABI-estável e manter leitura de Tile separada da lógica de Walker/Looter.
O fluxo de alto nível é Position Map::getTile Tile::things classificação Item/Creature. Walker, targeting e looter → → →
consomem essa visão, mas não devem conhecer a representação interna do vetor de things.
6. Item, Inventory e Container
A API recuperada inclui getContainerInfo, getContainerItems, getContainerAmount, containerUseItem, movimentos 
entre container/ground, close e back. Isso indica um ContainerManager independente do Looter.
Para nossa implementação: Item representa ID/subtype/count; Container representa metadados e uma coleção de 
Item; ContainerManager descobre containers abertos e executa operações. Looter somente fornece a política: o que 
pegar e para onde mover.
7. Targeting e combate
O corpus contém conceitos Hold Target, lastTargetID, Attack Any Monster, Should Attack Me, Keep Follow, Creature 
Range, Hpperc Start/End, Targeting Max Run Dist e Targeting Attack Delay. Isso sugere uma máquina de seleção e 
manutenção de alvo, não apenas uma chamada attack().
Nossa divisão recomendada: TargetSelector pontua/filtra snapshots; CombatController mantém target atual e executa
Attack/Follow; MovementController cuida de aproximação. Assim os offsets ficam completamente fora da política de 
targeting.
8. Walker / Cavebot
As strings recuperadas descrevem Walker como parte do cavebot que segue waypoints em loop, com Pathable IDs, 
Open Holes, delays, mudança de andar, distância e estados on/off/if-stuck. Isso aponta para um interpretador de 
nós/waypoints com ações especiais.
Nossa versão deve usar um Node/Waypoint tipado: Walk, Use, Rope, Shovel, Ladder, Wait, Label/Goto e 
Script/Condition. Pathfinding e execução ficam separados.
9. Looter / corpse pipeline
O material recuperado mostra Auto Loot, Open Bag, Hand Mode, Open After Kill, Skinning, Corpse Bag ID, Looter Max
Go Dist, Looter Ignore Mobs, Move Items Delay e Open Bp Delay. Há mensagens de estado para limites de tentativas, 
falta de capacidade, distância e objetos não-abertos.
<PARSED TEXT FOR PAGE: 5 / 88>
A arquitetura que se encaixa é: registrar morte/posição localizar candidato de corpse no tile validar → →
distância/openable Use detectar container aberto enumerar items aplicar LootRules Move lidar com → → → → → →
nested bags encerrar/voltar. O Crome também usa heurísticas textuais de corpse (dead, slain, remains, corpse, →
fallen, lifeless etc.), que devem ser tratadas como heurística, não como única fonte.
10. Healing e suporte
Além de Mana Shield, AntiPara e AutoHur, o corpus contém Heal Party, Low Health e No Mana Regen. A nossa 
camada deve separar Sensor (snapshot de player/party/conditions) de Rule (limiares) e Action (spell/rune/useWith).
11. API / scripting
O Crome expõe funções de alto nível como getCreatures e operações de container/mapa, além de 
memoryRead/memoryWrite. Para nossa implementação, a API pública deve privilegiar funções tipadas e seguras; 
acesso bruto à memória pode ficar em uma camada de diagnóstico, não como dependência dos módulos.
Exemplo conceitual: api.getCreatures() retorna snapshots; api.attack(id), api.walkTo(pos), api.use(item), 
api.getContainers(). Scripts não precisam saber moduleBase, RVA ou offsets.
12. Catálogo semântico do payload
As seções abaixo foram produzidas decodificando os 17.802 STR embutidos e associando-os às funções ofuscadas que 
referenciam essas constantes. Uma função pode aparecer em mais de uma seção.
Process / Memory
46 strings relacionadas; 30 funções com referências relacionadas.
 STR153: VirtualAllocEx
 STR161: OpenProcess
 STR182: WriteProcessMemory
 STR190: ReadProcessMemory
 STR552: VirtualProtectEx
 STR570: OpenProcessToken
 STR857: CreateToolhelp32Snapshot
 STR873: Process32FirstW
 STR881: Process32NextW
 STR10980: NtOpenProcess
 STR17264: memoryRead
 STR17265: memoryWrite
Funções relacionadas (nome ofuscado, linhas, quantidade de strings):
 QC L18142-L18146 strings=8
 QV L18147-L18173 strings=18
<PARSED TEXT FOR PAGE: 6 / 88>
 QB L18174-L18180 strings=11
 QN L18181-L18185 strings=9
 EX L19001-L19011 strings=6
 EC L19012-L19016 strings=8
 EV L19017-L19022 strings=8
 EB L19023-L19028 strings=9
 EN L19029-L19034 strings=10
 E3 L19085-L19091 strings=8
 E5 L19097-L19103 strings=9
 E6 L19104-L19142 strings=32
 E7 L19143-L19152 strings=13
 R6 L19622-L19641 strings=33
 UE L36608-L36633 strings=26
 IB L37692-L37760 strings=26
 AK L41815-L41891 strings=14
 AC L42068-L42192 strings=40
 A0 L42294-L42335 strings=22
 DS L45063-L45159 strings=89
 DF L45174-L45290 strings=90
 D1 L45675-L45682 strings=11
 D2 L45683-L45693 strings=13
 D3 L45694-L45701 strings=11
 FS L47687-L47784 strings=20
 FD L47785-L47820 strings=20
 FF L47821-L47837 strings=18
 FH L47852-L47888 strings=34
 F1 L48555-L48606 strings=9
 VV L64590-L64901 strings=195
Builds / Profiles
11 strings relacionadas; 0 funções com referências relacionadas.
 STR4996: nostalrius_profiles
 STR4997: olders_profiles
 STR5036: olders_april
 STR5037: olders_september
 STR5295: olders_opengl.exe
 STR5296: olders_dx9.exe
 STR5346: Download game client with OT Client version.
Creature / Battle List
94 strings relacionadas; 15 funções com referências relacionadas.
 STR6415: Party
 STR6416: Heal players from party group (green skulls)
 STR6435: Lecz ludzi z dru�yny party (zielone czaszki)
<PARSED TEXT FOR PAGE: 7 / 88>
 STR6454: When on screen appear player other than from Friends.txt then character make step in direction. 0 - 
north, 1 - east, 2 - south, 3 - west, Diagonally: 4 - north east, 5 - south east, 6 - south west, 7 - north west
 STR6558: Turn to random directions to avoid game logout.
 STR6591: Equip energy ring to equipment when your health percent will below e.g. [60%]. Dequip to any empty 
backpack if hpperc above 60%
 STR6648: Use rune id on attacked creature.
 STR6656: Turn our character to direction. 0 - north, 1 - east, 2 - south, 3 - west.
 STR6658: Step 1sqm to direction. 0 - north, 1 - east, 2 - south, 3 - west, Diagonally: 4 - north east, 5 - south east, 6 - 
south west, 7 - north west
 STR6666: Use rune id on last attacked creature.
 STR6668: Paralyse last attacked creature.
 STR6848: Step 1sqm to direction.
 STR6957: MinSelf HP: dont cast/shoot if your character hpperc will below this value.
 STR6958: Mobs HP: dont cast/shoot if monster hpperc is not in this range.
 STR6961: Monsters list: add creatures separated be comma. For stange names use Rat~ To mark all use *
 STR7010: Skin creatures using specific tool. Choose tool or enter ID then type monsters to skin.
 STR13941: none|cap|level|exp|item|players|monsters|npcs|distance|isopen|soul|hpperc|mpperc
 STR14318: Self.Step(Self.getDirectionFromPosition(STEP_POS[1], STEP_POS[2], STEP_POS[3]))
 STR14332: Description: Equip energy ring when when hpperc <= 75% else dquip to first open 
container.
 STR14336: local HPPERC = 75 -- when hpperc below or equal 
rquip ring else dequip
 STR14340: if hp <= HPPERC then
 STR14361: local players = Creature.iPlayers(9, false) -- get players with max range only on my floor
 STR15407: tuse 2268 - Use item with attacked creature.
 STR15411: step 0 - Step to direction [see above].
 STR15415: runelast 3198 - Use item id: 3198 with last attacked creature.
 STR15484: UH - Hpperc
 STR15508: LF - Hpperc
 STR15514: Friends Hpperc
 STR15517: Self Hpperc
 STR15518: Enter self minimal hpperc to enable heal friends.
 STR15535: Heal Party
 STR15536: Heal players inside party group.
 STR15537: Lecz graczy w dru�ycie party.
 STR15599: Auto shoot rune ID with lastTargetID player or monster. When selfHpperc < 50% avoid shooting
 STR15647: Play sound when your character hpperc will below typed value.
 STR15650: Enter self hpperc value when below will play sound.
 STR15683: Turn your character to random directions avoiding Game LogOut.
 STR15689: Enable step to direction when player detected on (range: x = [-7, 7], y = [-5, 5]). Safe list=Friends.txt
 STR15691: Step Direction
 STR15692: Direction to step when Player detected. 0 - north, 1 - east, 2 - south, 3 - west, 4 - north-east, 5 - south￾east, 6 - south-west, 7 - north-west.
 STR15710: Shoot Sudden Death Rune with lastTargetID depend on hpperc. When selfHpperc < 50% avoid 
shooting
 STR15712: SD Combo Hpperc
 STR15755: Auto follow attack on creatures from skill list. Press Esc to stop attack.
 STR15766: Creatures
<PARSED TEXT FOR PAGE: 8 / 88>
 STR15767: Write down creatures separated by comma to attack. Priority from left side. For modified names use: 
rat~
 STR15769: Hpperc Start
 STR15770: Attack creatures that health percent is above or equal value.
 STR15772: Hpperc End
 STR15773: Attack creatures that health percent is below or equal value.
 STR15775: Creature Range
 STR15776: Attack creatures with range (sqms near you)
 STR15779: Attack creatures only below or equal range. If you are knight set 1 else pally 3 or higher.
 STR15813: Start looting only if no creatures to attack.
 STR15816: Enable skinning creatures.
 STR16192: hpperc
 STR16988: Targeting: Failed to find attaking creature (
 STR16989: ) in all creatures list.
 STR16990: Targeting: Creature pos.z (
 STR16993: Targeting: Creature is not monster (
 STR16997: Targeting: Creature is not on screen (abs.x=
 STR17000: Targeting: Creature distance (
 STR17003: Targeting: Function: Should Attack Me is enabled and creature don't attacked me + player on screen.
 STR17004: Targeting: stop attack creature due max time.
 STR17005: Targeting: Creature hpperc (
 STR17064: Looter: set Creature looted due time
 STR17108: Looter: failed to get skinning creature.
 STR17121: Looter: failed to get skinning creature name
 STR17239: targetingSetHppercRange
 STR17241: looterAddCreature
 STR17297: selfDirection
 STR17313: selfUseItemWithCreature
 STR17336: getCreatures
 STR17337: creatureAttack
 STR17338: creatureReach
 STR17430: party
 STR17534: skull
 STR17538: direction
Funções relacionadas (nome ofuscado, linhas, quantidade de strings):
 HU L54175-L54271 strings=84
 JB L55113-L55221 strings=94
 LI L57379-L57436 strings=35
 LD L57516-L57677 strings=379
 ZH L58413-L58455 strings=31
 ZJ L58456-L58682 strings=192
 X0 L59739-L62394 strings=304
 CU L63086-L63325 strings=34
 CX L63540-L63604 strings=10
 C0 L63904-L64063 strings=17
 VV L64590-L64901 strings=195
 BA L65025-L65190 strings=84
<PARSED TEXT FOR PAGE: 9 / 88>
 WQW L65624-L65659 strings=12
 WQE L65660-L65694 strings=12
 WQN L65830-L65870 strings=12
Player / Stats / Conditions
282 strings relacionadas; 25 funções com referências relacionadas.
 STR100: ImpersonateSelf
 STR5641: utamo vita
 STR6223: utamo gran tempus
 STR6247: Q: Some functions doesn't works. e.g. dring mana fluids or eat food.
 STR6370: Light healing, use spell example: exura, when your percent health drop below config level e.g. Light 
[95] % [25] Mana [exura]
 STR6372: Medium healing, when your health level drop below e.g. 60%
 STR6374: Heavy damage healing, when your health level drop below e.g. 30%. Rifbot ignore in this case healing 
Light and Mid.
 STR6379: Lekkie leczenie, u�ywa czaru np. exura kiedy poziom �ycia spadnie poni�ej ustalonego % 
przyk�adowo: Light [95] % [25] Mana [exura]
 STR6387: Use rune Ulitimate Healing on your ground position self.x, self.y, self.z when your health level drop 
below e.g. 50%. Priority = 1.
 STR6389: Drinking mana fluid in the same way as use UH. Priority = 2, If character don't want drink fluids run 
Rifbot as Admin.
 STR6392: Mana Potion (Tibia 10.0)
 STR6393: Restore mana using potions, when mp percent will below x. Enter potion id or shortcut: mp, smp, gmp, 
ump.
 STR6396: U�ywa runki Ulitimate Healing Rune na pozycj� ziemi self.x, self.y, self.z kiedy poziom �ycia spadnie
poni�ej np 50%. Priorytet = 1.
 STR6398: Picie mana fluida w taki sam spos�b jak UH. Priorytet = 2, je�eli nie chce pi� manasa to odpal bota 
jako Admin.
 STR6406: Don't heal friends if percent health our character is below e.g. 80%
 STR6408: Heal using spell exura sio, bot checking mana needed to cast spell and magic level.
 STR6410: Heal using spell exura min sio, bot checking mana needed to cast spell and magic level.
 STR6427: Lecz za pomoc� exura sio, sprawdzana jest mana potrzebna do u�ycia czaru oraz mlv.
 STR6429: Lecz za pomoc� exura min sio, sprawdzana jest mana potrzebna do u�ycia czaru oraz mlv.
 STR6441: ON or OFF whole section with making runes/burning mana.
 STR6443: If mana percent % will below then.
 STR6445: Say this spell on Default channel. Bot checking mana to cast spell and magic level. Wearing blanks 
runes is enabled for games that require it.
 STR6452: Wypowiada ten czar na default kanale. Sprawdzana jest mana potrzebana do u�ycia czaru oraz mlv. 
Zak�ada blank rune, je�eli gra tego wymaga.
 STR6472: If you will don't have more balnk runes or soul points (Rifbot checking white message: You need a 
magic item to cast this spell) then
 STR6479: Je�eli nie b�dziesz mia� blank runek lub souli (sprawdzana jest bia�a wiadomo��: You need a 
magic item to cast this spell) wtedy
 STR6488: No Mana Regen
 STR6489: Play sound if no mana regeneration for 10 seconds and mana is not full.
 STR6490: Low Health
 STR6491: Play sound if low percent of health. Type percent in input.
<PARSED TEXT FOR PAGE: 10 / 88>
 STR6492: Self
 STR6498: D�wi�k jak mana nie regeneruje sie przez 10s oraz mana nie jest pe�na.
 STR6530: Mana Shield
 STR6531: Use spell utamo vita when character is out of (mana shield / ering). Rifbot checking for mana needed to 
cast spell and magic level.
 STR6533: Use spell from input e.g. exura when your character is under paralyse.
 STR6537: Use spell from input e.g. utani gran hur when you out of haste condition.
 STR6540: U�yj czaru utamo vita kiedy posta� nie ma tarczy many (mana shield / ering). Sprawdzana jest ilo��
many oraz mlv.
 STR6546: U�ywa czaru z pola obok np. utani gran hur kiedy nie jeste� pod wp�ywem kondycji haste.
 STR6591: Equip energy ring to equipment when your health percent will below e.g. [60%]. Dequip to any empty 
backpack if hpperc above 60%
 STR6602: If target health percent, that you attack drop below e.g. [30]% then character shoot SD with him.
 STR6608: Spam rune id with target. If your character health percent drop below 50% then won't shot runes.
 STR6650: Use rune on yourself e.g. UH.
 STR6668: Paralyse last attacked creature.
 STR6676: Use mana fluid on your character only if your mana percent is below 100%
 STR6678: Use life fluid on your character only if health percent is below 100%
 STR6680: Healing with UH only if health is not fully.
 STR6732: U�ywa mana fluida na swojej postaci tylko wtedy kiedy poziom % many jest mniejszy ni� 100%
 STR6753: How many days left to end of your key license. Rifbot stop itself when license expired. Right click to edit
key.
 STR6868: Add code for lua execution. Don't use loop and modules, just one line script example: if 
Self.ManaPercent() >= 95 then Self.CastSpell('exura', 25) end
 STR6928: Dodaj kod lua do wywo�ania. Nie u�ywaj p�tli ani modu��w np: if Self.ManaPercent() >= 95 then 
Self.CastSpell('exura', 25) end
 STR6956: MinMana: minimal mana to cast/shoot.
 STR6957: MinSelf HP: dont cast/shoot if your character hpperc will below this value.
 STR6963: Attack monsters when health percent will between [0] and [100]% If you want skilling on slime attack 
mother first decreasing little hp and later set [99] <> [100]%
 STR6965: Attack monsters only in range [amount of squares] from self character. Set [7] if you want attack 
monsters on whole screen.
 STR6982: MinMana: minimalna ilo�� many do u�ycia.
 STR6983: MinSelf HP: nie u�ywaj je�eli ilo�� % �ycia twojej postaci jest poni�ej.
 STR7192: manaFluidItem.count =
 STR13850: selfHealer
 STR13941: none|cap|level|exp|item|players|monsters|npcs|distance|isopen|soul|hpperc|mpperc
 STR14298: if Self.Amulet().id == 0 then -- get currnet amulet.id
 STR14299: Self.EquipItem(SLOT_AMULET, SSA, 1) -- equip stone skin amulet to amulet slot with quantity 1.
 STR14317: if Self.DistanceFromPosition(STEP_POS[1], STEP_POS[2], STEP_POS[3]) ~= 0 then
 STR14318: Self.Step(Self.getDirectionFromPosition(STEP_POS[1], STEP_POS[2], STEP_POS[3]))
 STR14338: local hp = Self.HealthPercent()
 STR14339: local ring = Self.Ring()
 STR14342: Self.EquipItem(SLOT_RING, ENERGY_RING.off, 1)
 STR14346: Self.DequipItem(SLOT_RING, 0, 19) -- dequip ring to last slot in first backpack.
 STR14360: if Self.isConnected() then
 STR14365: Self.Logout()
 STR14582: Mana
 STR14645: Mana Potion:
<PARSED TEXT FOR PAGE: 11 / 88>
 STR14648: umana
 STR14717: mana
 STR14718: lightmana
 STR14719: mana potion
 STR14720: manapotion
 STR14722: manafluid
 STR14723: mana fluid
 STR14725: strongmana
 STR14726: smana
 STR14727: strong mana fluid
Funções relacionadas (nome ofuscado, linhas, quantidade de strings):
 QH L18064-L18068 strings=4
 UX L36728-L36793 strings=5
 HU L54175-L54271 strings=84
 JB L55113-L55221 strings=94
 JM L55274-L55323 strings=12
 J2 L55404-L55455 strings=88
 J3 L55456-L55586 strings=87
 J7 L55788-L55816 strings=4
 KW L55839-L55864 strings=12
 KF L56223-L56299 strings=16
 KZ L56643-L56674 strings=27
 KN L56754-L56805 strings=24
 K6 L56930-L56941 strings=1
 LW L56963-L56971 strings=1
 LT L57064-L57291 strings=65
 LI L57379-L57436 strings=35
 LD L57516-L57677 strings=379
 LB L57814-L57881 strings=52
 ZH L58413-L58455 strings=31
 ZJ L58456-L58682 strings=192
 X0 L59739-L62394 strings=304
 X2 L62435-L62472 strings=27
 C0 L63904-L64063 strings=17
 VV L64590-L64901 strings=195
 BA L65025-L65190 strings=84
Map / Tile / Ground
34 strings relacionadas; 10 funções com referências relacionadas.
 STR6387: Use rune Ulitimate Healing on your ground position self.x, self.y, self.z when your health level drop 
below e.g. 50%. Priority = 1.
 STR6548: After look on item at ground or inside container appear message inside Rifbot panel-> Information Box 
about item id and ground position.
 STR6556: Using fishing rod on water tiles until capity reach 6oz or 28oz when SpearPickup is enabled. Required 
fishing rod inside equipment or container.
<PARSED TEXT FOR PAGE: 12 / 88>
 STR6822: Right click on Walker -> Settings -> Pathable Ids and there add ids of fire fields. You can check it by 
looking on ground fire and see Rifbot Information Box for results.
 STR6864: Open ground container on position and deposit there all items from list. It's good idea to call: restart 
bps function after this.
 STR6997: Ground
 STR6998: Throws items to the ground.
 STR14307: Description: Step to ground x, y, z when any dmg taken.
 STR14464: Background mode, click to restore.
 STR15423: mouseuse 3155 - Use itemid with ground under mouse cursor.
 STR15800: Drop Ground
 STR15801: Drop items from this list on the ground.
 STR16166: Enter items id to store inside ground container.
 STR16788: Walker: (lootbag) using ground container...
 STR16790: Walker: (lootbag) force use ground even if trashed
 STR16793: Walker: (lootbag) ground container is full, next to open not found.
 STR17092: Looter: Return true due max move to ground tries
 STR17126: Looter: ignore due outOfScreen
 STR17275: getGroundFromMouse
 STR17277: getMousePosFromGround
 STR17311: selfUseItemWithGround
 STR17339: getMap
 STR17340: mapGetTopMoveItem
 STR17341: mapGetAllTopItems
 STR17342: mapGetItems
 STR17343: mapUseItem
 STR17344: mapMoveItem
 STR17345: mapUseItemWithGround
 STR17350: containerUseItemWithGround
 STR17353: containerMoveItemToGround
Funções relacionadas (nome ofuscado, linhas, quantidade de strings):
 HU L54175-L54271 strings=84
 JF L54928-L54933 strings=1
 J3 L55456-L55586 strings=87
 LI L57379-L57436 strings=35
 LD L57516-L57677 strings=379
 ZD L58371-L58383 strings=4
 X0 L59739-L62394 strings=304
 CM L63648-L63895 strings=19
 C2 L64076-L64234 strings=6
 VV L64590-L64901 strings=195
Item / Inventory
371 strings relacionadas; 48 funções com referências relacionadas.
 STR135: struct;uint Mask;int Item;int SubItem;uint State;uint StateMask;ptr Text;int TextMax;int Image;lparam 
Param;
 STR139: ulong_ptr ItemData;ptr TypeData;uint CCH;handle BmpItem
<PARSED TEXT FOR PAGE: 13 / 88>
 STR258: SubItem
 STR271: Item
 STR287: item
 STR357: InsertMenuItemW
 STR364: GetMenuItemCount
 STR4962: area rune|ue spell|target spell|face exori vis|target rune|knight exori|pally mas san|mage mort hur
 STR6229: Rifbot has been created for automation bored things on Tibia 7.4 - 8.0 & 10.0 servers. Most of time bot is
used for making runes, skilling and as support in manual exp or pvp.
 STR6236: -s Runemaker.txt -> load settings from Rifbot-Project\Settings\
 STR6242: -s Runemaker.txt -> wczytuje ustawienia z Rifbot-Project\Settings\
 STR6247: Q: Some functions doesn't works. e.g. dring mana fluids or eat food.
 STR6272: Q: Niekt�re funkcje nie dzia�aj� np. picie fluida czy jedzienie fooda.
 STR6387: Use rune Ulitimate Healing on your ground position self.x, self.y, self.z when your health level drop 
below e.g. 50%. Priority = 1.
 STR6389: Drinking mana fluid in the same way as use UH. Priority = 2, If character don't want drink fluids run 
Rifbot as Admin.
 STR6391: Driniking life fluid. Priority = 3.
 STR6394: Items
 STR6396: U�ywa runki Ulitimate Healing Rune na pozycj� ziemi self.x, self.y, self.z kiedy poziom �ycia spadnie
poni�ej np 50%. Priorytet = 1.
 STR6398: Picie mana fluida w taki sam spos�b jak UH. Priorytet = 2, je�eli nie chce pi� manasa to odpal bota 
jako Admin.
 STR6400: Picie life fluida. Priorytet = 3.
 STR6412: Heal using UH (ultimate healing rune)
 STR6414: Heal using IH (intense healing rune)
 STR6431: Lecz za pomoc� runek UH (ultimate healing rune)
 STR6433: Lecz za pomoc� runek IH (intense healing rune)
 STR6441: ON or OFF whole section with making runes/burning mana.
 STR6445: Say this spell on Default channel. Bot checking mana to cast spell and magic level. Wearing blanks 
runes is enabled for games that require it.
 STR6452: Wypowiada ten czar na default kanale. Sprawdzana jest mana potrzebana do u�ycia czaru oraz mlv. 
Zak�ada blank rune, je�eli gra tego wymaga.
 STR6472: If you will don't have more balnk runes or soul points (Rifbot checking white message: You need a 
magic item to cast this spell) then
 STR6476: Instead of using spell for making runes character will use e.g. exura
 STR6479: Je�eli nie b�dziesz mia� blank runek lub souli (sprawdzana jest bia�a wiadomo��: You need a 
magic item to cast this spell) wtedy
 STR6487: Play sound if no more blanks and we want to create rune and message appear: You need a magic item 
to cast this spell.
 STR6496: D�wi�k jak nie mamy blanek a chcemy zrobi� runk� i pojawia si� wiadomo��: You need a magic
item to cast this spell.
 STR6547: LookItemID
 STR6548: After look on item at ground or inside container appear message inside Rifbot panel-> Information Box 
about item id and ground position.
 STR6566: Refill ammunition to specific slot: 5 - shield, 6 - weapon, 10 - arrow.
 STR6587: Uzupe�niaj amunicj� do wybranego slota: 5 - tarcza, 6 - bro�, 10 - strza�y.
 STR6604: Wear ID from input into ring slot when this is empty. Always set id of ring before usage.
 STR6606: Wear ID from input into amulet slot when this is empty.
 STR6607: RuneMax
<PARSED TEXT FOR PAGE: 14 / 88>
 STR6608: Spam rune id with target. If your character health percent drop below 50% then won't shot runes.
 STR6621: Spamuje rune w atakowany cel. Je�eli poziom �ycia twojej postaci spdanie poni�ej 50% nie b�dzie 
strzela�.
 STR6635: Q: How to check item ID?
 STR6636: A: Look on item (right click->look). Later check down of Rifbot panel Information Box.
 STR6644: Wear item into ring slot. Make sure you put id of not used item because some rings have different id 
when you wear them.
 STR6646: Wear item into amulet slot.
 STR6648: Use rune id on attacked creature.
 STR6650: Use rune on yourself e.g. UH.
 STR6652: Click use on item from equipment or backpack e.g. eating food.
 STR6654: [Only for classic Tibia] Use item with crosshairs.
 STR6660: Use rune id on character named: Ben.
 STR6664: Shoot Sudden Death Rune in last target.
 STR6665: RUNELAST 3160
 STR6666: Use rune id on last attacked creature.
 STR6672: Drop item under mouse cursor with amount = 1.
 STR6674: Drop two items under your character with amount = 2.
 STR6676: Use mana fluid on your character only if your mana percent is below 100%
 STR6677: LIFEFLUID
 STR6678: Use life fluid on your character only if health percent is below 100%
 STR6682: Use rune on square under mouse cursor.
 STR6684: Equip item to slot in equipment. 0 - helmet, 1 - backpack, 2 - amulet, 3 - armor, 4 - shield, 5 - weapon, 6 - 
legs, 7 - boots, 8 - ring, 9 - arrow.
 STR6720: Rzuca runk� SD - Sudden Death Rune w ostatni cel.
 STR6728: Rzuca item id po kursor myszki w ilo�ci 1.
 STR6730: Rzuca pod siebie na przemian 2 itemki w ilo�ci 2.
 STR6732: U�ywa mana fluida na swojej postaci tylko wtedy kiedy poziom % many jest mniejszy ni� 100%
 STR6734: U�ywa life fluida na swojej postaci tylko wtedy gdy poziom % �ycia b�dzie mniejszy ni� 100%
 STR6740: Zak�ada item id do slota w ekwipunku. 0 - helm, 1 - plecak, 2 - amulet, 3 - zbroja, 4 - tarcza, 5 - bro�, 6 -
spodnie, 7 - buty, 8 - pier�cionek, 9 - strza�y.
 STR6830: Go up using rope item. Add node first and then rope wpt. Rope ids you can change in Settings->Rope Id
 STR6854: Stand next to free depot spot. Use it first before depositing items.
 STR6855: Action Advanced: Deposit Items
 STR6856: Deposit items to locker or container inside. If you put 0 in first input then character deposit items direct
to locker else will search for container id to open.
 STR6857: Action Advanced: Withdraw Items
 STR6858: Withdraw items from depot to backpack. First input - from we withdrawing, second - where index of 
opened backpack (0 - first), third - amount to withdraw if 0 then to max cap/space.
 STR6861: Action Advanced: Sell Items | Trade
 STR6862: Stand near npc shop and sell all items. Type items {name, id} example: {Sabre, 2385}. On some servers 
NPC Trade panel is allowed and you will able to buy/sell items.
 STR6864: Open ground container on position and deposit there all items from list. It's good idea to call: restart 
bps function after this.
 STR6914: Sta� ko�o wolnej skrzynki depot. U�yj tego przed odk�adaniem itemk�w do depot.
 STR6922: Sta� przy npc i sprzedaj rzeczy. Ustaw itemy tak {nazwa, id} np: {Sabre, 2385}. Na cz�ci serwer�w 
dost�pny jest NPC Trade panel bot b�dzie m�g� kupi� lub sprzeda� itemki za jego pomoc�.
 STR6949: Opens single panel to set rune/spell attack.
 STR6952: AttackType: choose attack type you need. For rune set ID.
<PARSED TEXT FOR PAGE: 15 / 88>
 STR6953: Spell/Rune: type name of spell or rune ID.
Funções relacionadas (nome ofuscado, linhas, quantidade de strings):
 WF L18437-L18478 strings=14
 WH L18486-L18493 strings=4
 WK L18523-L18580 strings=18
 WL L18581-L18588 strings=4
 WV L18647-L18653 strings=3
 WM L18701-L18749 strings=18
 W4 L18775-L18794 strings=18
 W5 L18795-L18799 strings=4
 EQ L18837-L18855 strings=18
 UX L36728-L36793 strings=5
 AP L41662-L41702 strings=18
 AA L41703-L41739 strings=18
 HU L54175-L54271 strings=84
 JA L54879-L54897 strings=7
 JN L55227-L55273 strings=13
 JM L55274-L55323 strings=12
 J1 L55358-L55403 strings=14
 J2 L55404-L55455 strings=88
 J3 L55456-L55586 strings=87
 KW L55839-L55864 strings=12
 KY L56061-L56094 strings=29
 KP L56143-L56155 strings=2
 KD L56179-L56222 strings=37
 KF L56223-L56299 strings=16
 KN L56754-L56805 strings=24
 LE L56972-L57055 strings=73
 LT L57064-L57291 strings=65
 LI L57379-L57436 strings=35
 LD L57516-L57677 strings=379
 LB L57814-L57881 strings=52
 L1 L57923-L58027 strings=42
 ZI L58286-L58300 strings=7
 ZO L58301-L58319 strings=11
 ZP L58320-L58344 strings=11
 ZA L58345-L58357 strings=6
 ZD L58371-L58383 strings=4
 ZH L58413-L58455 strings=31
 ZJ L58456-L58682 strings=192
 XF L59377-L59468 strings=40
 XK L59497-L59575 strings=37
 X0 L59739-L62394 strings=304
 X2 L62435-L62472 strings=27
 CW L62651-L62978 strings=23
 VV L64590-L64901 strings=195
 BA L65025-L65190 strings=84
<PARSED TEXT FOR PAGE: 16 / 88>
 WQ_ L65876-L65917 strings=7
 WWW L66041-L66088 strings=6
 WEW L66623-L66833 strings=22
Container
74 strings relacionadas; 14 funções com referências relacionadas.
 STR6548: After look on item at ground or inside container appear message inside Rifbot panel-> Information Box 
about item id and ground position.
 STR6556: Using fishing rod on water tiles until capity reach 6oz or 28oz when SpearPickup is enabled. Required 
fishing rod inside equipment or container.
 STR6591: Equip energy ring to equipment when your health percent will below e.g. [60%]. Dequip to any empty 
backpack if hpperc above 60%
 STR6652: Click use on item from equipment or backpack e.g. eating food.
 STR6684: Equip item to slot in equipment. 0 - helmet, 1 - backpack, 2 - amulet, 3 - armor, 4 - shield, 5 - weapon, 6 - 
legs, 7 - boots, 8 - ring, 9 - arrow.
 STR6852: Close all containers and open main backpack. If you want open more containers add ids of backpacks 
separated by comma.
 STR6856: Deposit items to locker or container inside. If you put 0 in first input then character deposit items direct
to locker else will search for container id to open.
 STR6858: Withdraw items from depot to backpack. First input - from we withdrawing, second - where index of 
opened backpack (0 - first), third - amount to withdraw if 0 then to max cap/space.
 STR6864: Open ground container on position and deposit there all items from list. It's good idea to call: restart 
bps function after this.
 STR6994: Collecting items to backpack. List of items to loot you can set clicking button [Items]
 STR6996: Opens panel with loot list. Index: 0 means first opened backpack, 1 - second container etc. Add items 
separated by comma.
 STR7000: Convert 100 gps to 1 platinum and 100 platinum to 1 crystal coin using backpack slot.
 STR7004: Loot items without opening containers (like old Rifbot)
 STR7006: Open next backpack with uh runes from index where last rune was used.
 STR13970: Pathable ID|Rope ID|Shovel ID|Pick ID|Mine Pick ID|Machete ID|Counter ID|Depot ID|Ladder ID|
Food ID|Rock ID|Open Holes ID|Non Pathable ID|Walk Tries|Re Walk Delay|Open Bp Delay|Reach Dp Tries|
Use Tries|Tool Delay|Walker After Kill Delay|Ignore Dp Time|Lure Step Time|Change Floor Delay|
MagicShooter Rune Delay|MagicShooter Spell Delay|MagicShooter Extra Safe Range|Looter Ignore Mobs|Move 
Items Delay|Targeting Attack Delay|Looter Module Delay|Fluids Heal Rune Delay|Walk Through Players|Map 
Click Walking|Targeting Max Run Dist|Corpse Bag ID|Looter Max Go Dist
 STR14332: Description: Equip energy ring when when hpperc <= 75% else dquip to first open 
container.
 STR14346: Self.DequipItem(SLOT_RING, 0, 19) -- dequip ring to last slot in first backpack.
 STR14828: Enter ids separated by comma. Index: 0 = first backpack.
 STR14928: [Looter->Hand Mode]: Next backpack to open for loot not found.
 STR15424: wear 3031, 5 - Wear item id to equipment slot: 0 - head, 1 - backpack, 2 - amulet, 3 - armor , 4 - 
shield, 5 - weapon, 6 - legs, 7 - feet, 8 - ring, 9 - arrow.
 STR15578: Enter id of ring to equip. Put id not used ring from container
 STR15593: Loot items from opened containers. Click button <Loot> for config.
 STR15668: Logout your character when out of blank runes or soul points. Read proxy, works even with closed 
backpacks. Use easy with player logout or relogin.
 STR15671: Cast spell when out of blank runes or soul points. Read proxy, works even with closed backpacks.
<PARSED TEXT FOR PAGE: 17 / 88>
 STR15719: Fishing until reach 6 cap or 28 cap if SpearPickup is enabled, fishing rod required in eq or inside 
opened container.
 STR15804: Convert 100 gps to 1 platinum and 100 platinum to 1 crystal coins clicking on container slot.
 STR15825: Open next bp of Uhs from last using uh container. Only when healing function called.
 STR16048: Open Bp Delay:
 STR16123: Enter bps id to open from main backpack.
 STR16127: To backpack id inside depot (if 0 will deposit to locker)
 STR16136: To container index (0 = first open bp, 1 = second bp):
 STR16166: Enter items id to store inside ground container.
 STR16479: corpse bag id
 STR16487: open bp delay
 STR16522: Corpse Bag ID
 STR16530: Open Bp Delay
 STR16657: containerisopen
 STR16676: Walker: main backpack not found.
 STR16677: Walker: something wrong with backpacks
 STR16678: Walker: there is no enough containers id
 STR16679: inside main backpack. You set
 STR16681: Walker: container
 STR16682: not found in main backpack.
 STR16712: Walker: (deposit items) there is no backpack id
 STR16716: Walker: (deposit items) destiantion container is full. Cannot find next backpack id
 STR16718: Walker: (deposit items) destination container is full, next to open not found.
 STR16751: Walker: (withdraw items) there is no backpack id
 STR16753: Walker: (withdraw items) Container to (
 STR16757: Walker: (withdraw items 2) destination container is full, next to open not found.
 STR16758: Walker: (withdraw items) destination container is full, next to open not found.
 STR16769: Walker: (trade sell) All backpacks are closed, open one at least.
 STR16787: Walker: (lootbag) depositfrom containers are closed.
 STR16788: Walker: (lootbag) using ground container...
 STR16793: Walker: (lootbag) ground container is full, next to open not found.
 STR16859: Walker: (trade buy) All backpacks are closed, open one at least.
 STR17347: getContainerInfo
 STR17348: getContainerItems
 STR17349: getContainerAmount
 STR17350: containerUseItemWithGround
 STR17351: containerUseItem
 STR17352: containerMoveItemToContainer
 STR17353: containerMoveItemToGround
 STR17354: containerClose
 STR17355: containerBack
 STR17356: containerUseItemWithContainer
 STR17357: containerGetWithEmptySlots
Funções relacionadas (nome ofuscado, linhas, quantidade de strings):
 HU L54175-L54271 strings=84
 J3 L55456-L55586 strings=87
 KR L55878-L56043 strings=14
<PARSED TEXT FOR PAGE: 18 / 88>
 LI L57379-L57436 strings=35
 LD L57516-L57677 strings=379
 L2 L58028-L58109 strings=33
 ZU L58273-L58285 strings=4
 ZI L58286-L58300 strings=7
 ZO L58301-L58319 strings=11
 ZD L58371-L58383 strings=4
 XF L59377-L59468 strings=40
 XK L59497-L59575 strings=37
 X0 L59739-L62394 strings=304
 VV L64590-L64901 strings=195
Combat / Targeting
143 strings relacionadas; 31 funções com referências relacionadas.
 STR4961: none|stand near|follow|3sqm|4sqm|diagonal|diagonal 3sqm|face|2sqm
 STR6505: Player Attack
 STR6506: Play sound if other player not from Friends.txt attack us. Signal is single and play when we receive 
damage or not.
 STR6588: Hold Target
 STR6589: Attacking last aim when he disappear (change floor or go out of screen). To stop attack press key ESC.
 STR6602: If target health percent, that you attack drop below e.g. [30]% then character shoot SD with him.
 STR6640: Exiva last attacked target if it was player.
 STR6648: Use rune id on attacked creature.
 STR6666: Use rune id on last attacked creature.
 STR6668: Paralyse last attacked creature.
 STR6670: If you hold pressed key with this function attacking player and he move then in his previous spot bot 
throw magic wall.
 STR6820: Walker is part of cavebot module. It allows you to follow waypoints in loop.
 STR6866: Change state of module: walker, looter, cavebot - {off, on}, targeting - {on, off, if stuck - attack monsters 
only if stuck}, lure - {on, off} enabled or disable lure mode
 STR6926: Zmie� status modu�u: walker, looter, cavebot - {off, on}, targeting - {on, off, if stuck - atakuj potwory 
jak posta� si� zatrzyma�a}, lure - {on, off} w��cz lub wy��cz lurowanie potwork�w.
 STR6941: Attack mode, for knights best will be follow and diagonal, for palladins 3/4sqm or diagonal 3sqm, for 
mage 3sqm or face for exori vis casting.
 STR6942: Attack Any Monster
 STR6943: Character will attack any monster no matter if it's Demon or Rabbit. Priority by monster range.
 STR6944: Should Attack Me
 STR6945: Attack monster attacked you or any if no players on screen.
 STR6946: Attack Monster
 STR6947: Monsters to attack, separate names by comma. Priority from left to right side of input. For strange 
names use: rat~ If you want attack all monsters set enabled func: Attack Any Monsters.
 STR6949: Opens single panel to set rune/spell attack.
 STR6952: AttackType: choose attack type you need. For rune set ID.
 STR6959: Mode: set attack mode to this when rule will meet requirements.
 STR6963: Attack monsters when health percent will between [0] and [100]% If you want skilling on slime attack 
mother first decreasing little hp and later set [99] <> [100]%
<PARSED TEXT FOR PAGE: 19 / 88>
 STR6965: Attack monsters only in range [amount of squares] from self character. Set [7] if you want attack 
monsters on whole screen.
 STR6967: Typ atakowania, dla wojownik�w najlepsze b�dzie follow oraz diagonal (po skosie), dla paladyn�w 
utrzymywanie 3-4 kratek dystansu lub 3 kratki po skosie, dla mag�w 3sqm dystans lub face do ustawiania si� 
pod exori vis
 STR6973: Potworki do atakowania. Oddziel nazwy przecinkami. Priorytet atakowania od lewej do prawej strony 
pola. Dla dziwnych nazw u�yj: rat~ Je�eli chcesz aby posta� atakowa�a wszystkie potwory zaznacz funkcj�: 
Attack Any Monsters.
 STR6978: AttackType: wybierz regu�e atakowania. Dla rune pami�taj o wpisaniu ID.
 STR6992: Targeting
 STR7008: Open cropses only when no more monsters to attack.
 STR8218: follow
 STR13970: Pathable ID|Rope ID|Shovel ID|Pick ID|Mine Pick ID|Machete ID|Counter ID|Depot ID|Ladder ID|
Food ID|Rock ID|Open Holes ID|Non Pathable ID|Walk Tries|Re Walk Delay|Open Bp Delay|Reach Dp Tries|
Use Tries|Tool Delay|Walker After Kill Delay|Ignore Dp Time|Lure Step Time|Change Floor Delay|
MagicShooter Rune Delay|MagicShooter Spell Delay|MagicShooter Extra Safe Range|Looter Ignore Mobs|Move 
Items Delay|Targeting Attack Delay|Looter Module Delay|Fluids Heal Rune Delay|Walk Through Players|Map 
Click Walking|Targeting Max Run Dist|Corpse Bag ID|Looter Max Go Dist
 STR14425: Cavebot: Stopping walker, targeting, looter due character death.
 STR14461: attack rune
 STR15133: Player Attack.mp3
 STR15144: just attacked you.
 STR15172: unless you follow
 STR15403: exivat - Exiva current target or last attacked player.
 STR15407: tuse 2268 - Use item with attacked creature.
 STR15415: runelast 3198 - Use item id: 3198 with last attacked creature.
 STR15545: Re-attack last target id. [Bump Esc.key to stop]
 STR15599: Auto shoot rune ID with lastTargetID player or monster. When selfHpperc < 50% avoid shooting
 STR15611: Play sound & flash game window if someone attack you. Ignore friends list.
 STR15710: Shoot Sudden Death Rune with lastTargetID depend on hpperc. When selfHpperc < 50% avoid 
shooting
 STR15748: Targeting Enabled
 STR15749: Enable targeting mod.
 STR15754: Keep Follow
 STR15755: Auto follow attack on creatures from skill list. Press Esc to stop attack.
 STR15756: Utrzymuj follow za atakowan� kreatur�. Naci�nij ESC aby przesta� atakowa�.
 STR15758: Attack all monsters without adding them to list.
 STR15761: Attack monsters only attacking you or all if no players on screen.
 STR15767: Write down creatures separated by comma to attack. Priority from left side. For modified names use: 
rat~
 STR15769: Hpperc Start
 STR15770: Attack creatures that health percent is above or equal value.
 STR15772: Hpperc End
 STR15773: Attack creatures that health percent is below or equal value.
 STR15776: Attack creatures with range (sqms near you)
 STR15779: Attack creatures only below or equal range. If you are knight set 1 else pally 3 or higher.
 STR15813: Start looting only if no creatures to attack.
 STR16171: targeting|walker|looter|cavebot|lure
 STR16172: targeting
<PARSED TEXT FOR PAGE: 20 / 88>
 STR16492: after attack delay
 STR16501: targeting attack delay
 STR16506: targeting max run dist
 STR16535: After Attack Delay
 STR16544: Targeting Attack Delay
 STR16549: Targeting Max Run Dist
 STR16575: Walker (set state) invalid param for module. Must be looter, walker or targeting.
 STR16879: Attack monsters:
 STR16966: MagicShooter: Friend has low hp temp stop attacking.
 STR16988: Targeting: Failed to find attaking creature (
 STR16990: Targeting: Creature pos.z (
 STR16993: Targeting: Creature is not monster (
 STR16995: Targeting: Reached max (
 STR16996: ) sqms range. Stopping attack to avoid luring new monsters.
 STR16997: Targeting: Creature is not on screen (abs.x=
 STR17000: Targeting: Creature distance (
 STR17003: Targeting: Function: Should Attack Me is enabled and creature don't attacked me + player on screen.
 STR17004: Targeting: stop attack creature due max time.
Funções relacionadas (nome ofuscado, linhas, quantidade de strings):
 OW L38409-L39009 strings=427
 HL L54458-L54496 strings=1
 JA L54879-L54897 strings=7
 KY L56061-L56094 strings=29
 KZ L56643-L56674 strings=27
 KC L56687-L56704 strings=5
 KB L56711-L56753 strings=18
 KN L56754-L56805 strings=24
 LI L57379-L57436 strings=35
 LD L57516-L57677 strings=379
 LB L57814-L57881 strings=52
 ZF L58384-L58400 strings=9
 ZG L58401-L58412 strings=8
 XF L59377-L59468 strings=40
 XK L59497-L59575 strings=37
 X0 L59739-L62394 strings=304
 X1 L62395-L62434 strings=29
 X2 L62435-L62472 strings=27
 X5 L62523-L62572 strings=11
 CW L62651-L62978 strings=23
 CU L63086-L63325 strings=34
 CO L63338-L63349 strings=2
 CS L63360-L63368 strings=2
 CF L63380-L63392 strings=10
 CH L63396-L63477 strings=20
 VV L64590-L64901 strings=195
 BA L65025-L65190 strings=84
 WQW L65624-L65659 strings=12
<PARSED TEXT FOR PAGE: 21 / 88>
 WQE L65660-L65694 strings=12
 WQN L65830-L65870 strings=12
 WER L66842-L66861 strings=5
Walker / Cavebot
162 strings relacionadas; 23 funções com referências relacionadas.
 STR6513: Walker Stuck
 STR6514: Play sound if walker cannot find way to reach goal.
 STR6529: Wywo�uje d�wi�k jak walker nie mo�e odszuka� drogi do celu.
 STR6690: Enable or disable recording nodes in walker.
 STR6746: Rozpoczyna lub ko�czy nagrywanie nod�w w walkerze.
 STR6819: What is walker?
 STR6820: Walker is part of cavebot module. It allows you to follow waypoints in loop.
 STR6822: Right click on Walker -> Settings -> Pathable Ids and there add ids of fire fields. You can check it by 
looking on ground fire and see Rifbot Information Box for results.
 STR6826: Standard waypoint for walking on places different than 1sqm road. Character will try reach position 
with range 1 square from node. Weak node = 2 square.
 STR6828: To reach this wpt character position must be this same as waypoint. Use it for stairs, teleports, holes 
and close places. You can also record stands every 1 sqm.
 STR6832: Open hole using shovel and go down. If character open hole but stay then add hole id to walker-
>settings->open hole id
 STR6850: Wait for next waypoint in miliseconds. 1000ms = 1s
 STR6866: Change state of module: walker, looter, cavebot - {off, on}, targeting - {on, off, if stuck - attack monsters 
only if stuck}, lure - {on, off} enabled or disable lure mode
 STR6874: Open console for edit waypoints in source code.
 STR6876: Open change walker ids and bot delays console.
 STR6878: Delete all waypoints from list.
 STR6879: Co to walker?
 STR6880: Walker jest cz�ci� modu�u cavebota. Pozwala na osi�ganie wyznaczonych punkt�w w p�tli.
 STR6882: Naci�nij prawym myszki na Walker -> Settings -> Pathable Ids i tam dodaj id fire field�w. Mo�esz je 
sprawdzi� daj�c look na ziemie i nast�pnie sprawdzaj�c Rifbot Information.
 STR6892: Otwieraj dziur� u�ywaj�c szpadla i zejd� do niej. Je�eli posta� nie zchodzi dodaj id dziury do 
walker->settings->open holes id
 STR6926: Zmie� status modu�u: walker, looter, cavebot - {off, on}, targeting - {on, off, if stuck - atakuj potwory 
jak posta� si� zatrzyma�a}, lure - {on, off} w��cz lub wy��cz lurowanie potwork�w.
 STR6932: Wybierz kondycje, kt�ra musi si� spe�ni� aby walker m�g� przeskoczy� do danej flagi.
 STR6939: Walker
 STR13827: Walker Settings
 STR13836: Edit Waypoints
 STR13970: Pathable ID|Rope ID|Shovel ID|Pick ID|Mine Pick ID|Machete ID|Counter ID|Depot ID|Ladder ID|
Food ID|Rock ID|Open Holes ID|Non Pathable ID|Walk Tries|Re Walk Delay|Open Bp Delay|Reach Dp Tries|
Use Tries|Tool Delay|Walker After Kill Delay|Ignore Dp Time|Lure Step Time|Change Floor Delay|
MagicShooter Rune Delay|MagicShooter Spell Delay|MagicShooter Extra Safe Range|Looter Ignore Mobs|Move 
Items Delay|Targeting Attack Delay|Looter Module Delay|Fluids Heal Rune Delay|Walk Through Players|Map 
Click Walking|Targeting Max Run Dist|Corpse Bag ID|Looter Max Go Dist
 STR14425: Cavebot: Stopping walker, targeting, looter due character death.
 STR15209: Walker Stuck.wav
<PARSED TEXT FOR PAGE: 22 / 88>
 STR15427: record - Enable or disable recording nodes in walker.
 STR15644: Play sound when walker will no able to reach waypoint some time (stuck)
 STR15645: Wywo�aj d�wi�k kiedy walker nie b�dzie wstanie osi�gn�� wpt w okre�lonym czasie (utknie)
 STR15970: Cavebot
 STR15979: cavebot.ico
 STR16035: Pathable ID:
 STR16055: Pathable ID
 STR16171: targeting|walker|looter|cavebot|lure
 STR16182: walker
 STR16219: Walker: Login to game to add waypoint.
 STR16324: Walker: (goto) invalid param for position. Try 32131, 34232, 7
 STR16325: Walker: (goto) Invalid function name.
 STR16326: Walker: (goto) Enter label name first.
 STR16410: Clear Waypoints
 STR16412: Walker (goto) failed to find label:
 STR16469: Successfully applied walker settings.
 STR16470: Ustawienia dla walkera zosta�y zaaplikowane pomy�lnie.
 STR16476: pathable id
 STR16484: non pathable id
 STR16491: walker after kill delay
 STR16494: lure step time
 STR16509: Successfully applied walker settings for:
 STR16510: Zaaplikowano pomy�lnie ustawienia walkera dla:
 STR16527: Non Pathable ID
 STR16534: Walker After Kill Delay
 STR16537: Lure Step Time
 STR16553: Walker luring
 STR16557: Walker: Ignore wpt due max tries
 STR16558: Walker: Ignore wpt due max use tries
 STR16573: cavebot
 STR16574: lure
 STR16575: Walker (set state) invalid param for module. Must be looter, walker or targeting.
 STR16587: Walker: Key item not found.
 STR16592: Walker: Rope item not found.
 STR16603: Walker:
 STR16615: Walker: failed to split goto wpt
 STR16630: Walker: (goto) invalid sign for function. Try: >=, <=, >, <, =
 STR16632: Walker: (goto) invalid param value for function.
 STR16642: Walker: (goto) invalid param id for function.
 STR16655: Walker: (goto) invalid param position. Try 32131, 34232, 7
 STR16670: Walker: (wait) invalid param for wait time.
 STR16676: Walker: main backpack not found.
 STR16677: Walker: something wrong with backpacks
 STR16678: Walker: there is no enough containers id
 STR16681: Walker: container
 STR16684: Walker: Failed to get stand pos from depot id.
 STR16685: Walker: (reach depot) cannot find any depot.
 STR16688: Walker: (deposit items) Invalid amount of items.
 STR16691: Walker: (deposit items) failed to get destination id type: {id}.
<PARSED TEXT FOR PAGE: 23 / 88>
 STR16694: Walker: (deposit items) failed to found depot around me.
 STR16706: Walker: (deposit items) depot chest (
 STR16712: Walker: (deposit items) there is no backpack id
Funções relacionadas (nome ofuscado, linhas, quantidade de strings):
 HL L54458-L54496 strings=1
 KZ L56643-L56674 strings=27
 LQ L56957-L56962 strings=1
 LI L57379-L57436 strings=35
 LD L57516-L57677 strings=379
 LB L57814-L57881 strings=52
 L0 L57919-L57922 strings=2
 L1 L57923-L58027 strings=42
 L2 L58028-L58109 strings=33
 L3 L58110-L58124 strings=4
 ZF L58384-L58400 strings=9
 ZG L58401-L58412 strings=8
 ZJ L58456-L58682 strings=192
 ZL L58692-L58699 strings=2
 XE L58958-L59005 strings=1
 XD L59274-L59376 strings=14
 XF L59377-L59468 strings=40
 XK L59497-L59575 strings=37
 XB L59696-L59704 strings=2
 X0 L59739-L62394 strings=304
 VV L64590-L64901 strings=195
 BA L65025-L65190 strings=84
 WEW L66623-L66833 strings=22
Looter / Corpse
115 strings relacionadas; 19 funções com referências relacionadas.
 STR6863: Action Advanced: Lootbag
 STR6866: Change state of module: walker, looter, cavebot - {off, on}, targeting - {on, off, if stuck - attack monsters 
only if stuck}, lure - {on, off} enabled or disable lure mode
 STR6926: Zmie� status modu�u: walker, looter, cavebot - {off, on}, targeting - {on, off, if stuck - atakuj potwory 
jak posta� si� zatrzyma�a}, lure - {on, off} w��cz lub wy��cz lurowanie potwork�w.
 STR6993: Looting
 STR6994: Collecting items to backpack. List of items to loot you can set clicking button [Items]
 STR6996: Opens panel with loot list. Index: 0 means first opened backpack, 1 - second container etc. Add items 
separated by comma.
 STR7002: Opens bags inside dead cropses.
 STR7004: Loot items without opening containers (like old Rifbot)
 STR7009: Skinning
 STR7012: Zbieranie przedmiot�w do plecaka. List� przedmiot�w mo�esz ustawi� po klikni�ciu guzika [Loot
List]
 STR7014: Otwiera panel z list� loota. Index: 0 oznacza pierwszy otwarty plecak, 1 - drugi plecak etc. Dodawaj 
przedmioty oddzielaj�c je przecinkami.
<PARSED TEXT FOR PAGE: 24 / 88>
 STR7029: Looter
 STR13824: Loot list
 STR13843: Lootbag
 STR13858: autoLoot
 STR13899: hunters knife|skinpeeler|skinning knife
 STR13970: Pathable ID|Rope ID|Shovel ID|Pick ID|Mine Pick ID|Machete ID|Counter ID|Depot ID|Ladder ID|
Food ID|Rock ID|Open Holes ID|Non Pathable ID|Walk Tries|Re Walk Delay|Open Bp Delay|Reach Dp Tries|
Use Tries|Tool Delay|Walker After Kill Delay|Ignore Dp Time|Lure Step Time|Change Floor Delay|
MagicShooter Rune Delay|MagicShooter Spell Delay|MagicShooter Extra Safe Range|Looter Ignore Mobs|Move 
Items Delay|Targeting Attack Delay|Looter Module Delay|Fluids Heal Rune Delay|Walk Through Players|Map 
Click Walking|Targeting Max Run Dist|Corpse Bag ID|Looter Max Go Dist
 STR14425: Cavebot: Stopping walker, targeting, looter due character death.
 STR14855: obsidian knife|blunt skinning knife|wooden stake
 STR14875: AutoLooting
 STR14876: Loot List
 STR14928: [Looter->Hand Mode]: Next backpack to open for loot not found.
 STR15590: Enable looter mod.
 STR15592: Auto Loot
 STR15593: Loot items from opened containers. Click button <Loot> for config.
 STR15594: Zbieraj przedmioty do plecaka z innych otwartych pojemnik�w, cia� etc. Kliknij guzik <Loot> aby 
ustwi� co bot ma zbiera�.
 STR15596: {Ctrl+J} Open list with looting ids.
 STR15597: {Ctrl+J} Otwiera list� do ustawienia loota.
 STR15807: Opens bags inside dead corpses when no item to collect found.
 STR15810: Loot only when you open corpse manually.
 STR15813: Start looting only if no creatures to attack.
 STR15814: Zacznij lootowa� tylko gdy nie ma potwor�w do atakowania.
 STR15816: Enable skinning creatures.
 STR15818: Skinning Tool
 STR15819: Select skinning tool or type ID.
 STR15821: Skinning mobs
 STR16012: lootbag
 STR16171: targeting|walker|looter|cavebot|lure
 STR16359: lootbag:
 STR16479: corpse bag id
 STR16499: looter ignore mobs
 STR16502: looter module delay
 STR16507: looter max go dist
 STR16522: Corpse Bag ID
 STR16542: Looter Ignore Mobs
 STR16545: Looter Module Delay
 STR16550: Looter Max Go Dist
 STR16572: looter
 STR16575: Walker (set state) invalid param for module. Must be looter, walker or targeting.
 STR16784: lootbag:
 STR16786: Walker: (lootbag) failed to split items.
 STR16787: Walker: (lootbag) depositfrom containers are closed.
 STR16788: Walker: (lootbag) using ground container...
 STR16789: Walker: (lootbag) removing trash.
<PARSED TEXT FOR PAGE: 25 / 88>
 STR16790: Walker: (lootbag) force use ground even if trashed
 STR16793: Walker: (lootbag) ground container is full, next to open not found.
 STR17064: Looter: set Creature looted due time
 STR17074: Looter: return true due max use tries
 STR17087: Looter: Return true due max move tries
 STR17089: Looter: Return true due no capity
 STR17090: Looter: Return true due max move tries(2)
 STR17091: Looter: Return true due no capity (2)
 STR17092: Looter: Return true due max move to ground tries
 STR17095: dead
 STR17096: slain
 STR17098: remains
 STR17102: loot
 STR17105: lifeless
 STR17106: fallen
 STR17107: corpse
 STR17108: Looter: failed to get skinning creature.
 STR17114: skinning knife
 STR17118: blunt skinning knife
 STR17120: Looter: invalid id of skinning tool.
 STR17121: Looter: failed to get skinning creature name
 STR17122: Looter: Skinning tool not found.
 STR17123: Looter: ignore skinning due out of screen
 STR17124: Looter: ignore skinning due max use tries.
 STR17126: Looter: ignore due outOfScreen
 STR17127: Looter: ignore due ladder
Funções relacionadas (nome ofuscado, linhas, quantidade de strings):
 HL L54458-L54496 strings=1
 J3 L55456-L55586 strings=87
 KR L55878-L56043 strings=14
 LD L57516-L57677 strings=379
 LB L57814-L57881 strings=52
 L1 L57923-L58027 strings=42
 ZF L58384-L58400 strings=9
 ZJ L58456-L58682 strings=192
 XF L59377-L59468 strings=40
 XK L59497-L59575 strings=37
 X0 L59739-L62394 strings=304
 CX L63540-L63604 strings=10
 CM L63648-L63895 strings=19
 C_ L63896-L63903 strings=15
 C0 L63904-L64063 strings=17
 C2 L64076-L64234 strings=6
 VC L64523-L64589 strings=28
 VV L64590-L64901 strings=195
 BA L65025-L65190 strings=84
<PARSED TEXT FOR PAGE: 26 / 88>
Healing / Support
156 strings relacionadas; 18 funções com referências relacionadas.
 STR6247: Q: Some functions doesn't works. e.g. dring mana fluids or eat food.
 STR6272: Q: Niekt�re funkcje nie dzia�aj� np. picie fluida czy jedzienie fooda.
 STR6370: Light healing, use spell example: exura, when your percent health drop below config level e.g. Light 
[95] % [25] Mana [exura]
 STR6372: Medium healing, when your health level drop below e.g. 60%
 STR6374: Heavy damage healing, when your health level drop below e.g. 30%. Rifbot ignore in this case healing 
Light and Mid.
 STR6387: Use rune Ulitimate Healing on your ground position self.x, self.y, self.z when your health level drop 
below e.g. 50%. Priority = 1.
 STR6396: U�ywa runki Ulitimate Healing Rune na pozycj� ziemi self.x, self.y, self.z kiedy poziom �ycia spadnie
poni�ej np 50%. Priorytet = 1.
 STR6404: Healing friends. Percent below we heal our friends from list inside file: Friends.txt
 STR6406: Don't heal friends if percent health our character is below e.g. 80%
 STR6408: Heal using spell exura sio, bot checking mana needed to cast spell and magic level.
 STR6410: Heal using spell exura min sio, bot checking mana needed to cast spell and magic level.
 STR6412: Heal using UH (ultimate healing rune)
 STR6414: Heal using IH (intense healing rune)
 STR6416: Heal players from party group (green skulls)
 STR6418: Heal nicks from list Friends.txt available inside Rifbot folder.
 STR6431: Lecz za pomoc� runek UH (ultimate healing rune)
 STR6433: Lecz za pomoc� runek IH (intense healing rune)
 STR6490: Low Health
 STR6491: Play sound if low percent of health. Type percent in input.
 STR6532: AntiPara
 STR6536: AutoHur
 STR6563: Food Eater
 STR6564: Eating fish, meat, mushrooms and much more food in random time.
 STR6591: Equip energy ring to equipment when your health percent will below e.g. [60%]. Dequip to any empty 
backpack if hpperc above 60%
 STR6602: If target health percent, that you attack drop below e.g. [30]% then character shoot SD with him.
 STR6608: Spam rune id with target. If your character health percent drop below 50% then won't shot runes.
 STR6652: Click use on item from equipment or backpack e.g. eating food.
 STR6678: Use life fluid on your character only if health percent is below 100%
 STR6680: Healing with UH only if health is not fully.
 STR6708: Klika use na przedmiot z plecaka b�d� eq np. jedzenie fooda.
 STR6963: Attack monsters when health percent will between [0] and [100]% If you want skilling on slime attack 
mother first decreasing little hp and later set [99] <> [100]%
 STR13850: selfHealer
 STR13851: healerFriends
 STR13970: Pathable ID|Rope ID|Shovel ID|Pick ID|Mine Pick ID|Machete ID|Counter ID|Depot ID|Ladder ID|
Food ID|Rock ID|Open Holes ID|Non Pathable ID|Walk Tries|Re Walk Delay|Open Bp Delay|Reach Dp Tries|
Use Tries|Tool Delay|Walker After Kill Delay|Ignore Dp Time|Lure Step Time|Change Floor Delay|
MagicShooter Rune Delay|MagicShooter Spell Delay|MagicShooter Extra Safe Range|Looter Ignore Mobs|Move 
Items Delay|Targeting Attack Delay|Looter Module Delay|Fluids Heal Rune Delay|Walk Through Players|Map 
Click Walking|Targeting Max Run Dist|Corpse Bag ID|Looter Max Go Dist
 STR14338: local hp = Self.HealthPercent()
<PARSED TEXT FOR PAGE: 27 / 88>
 STR14459: heal rune
 STR14460: healing rune
 STR14576: Healing
 STR14577: healing.ico
 STR14739: health
 STR14740: lighthealth
 STR14741: health potion
 STR14743: stronghealth
 STR14744: shealth
 STR14745: strong health potion
 STR14747: greathealth
 STR14748: ghealth
 STR14749: great health potion
 STR14751: ultimatehealth
 STR14752: uhealth
 STR14753: ultimate health potion
 STR14756: supremehealth
 STR14757: suhealth
 STR14758: sphealth
 STR14770: smallhealth
 STR14772: small health potion
 STR14792: evil ultime healing rune
 STR14793: evil ultimatehealingrune
 STR14796: strong healing rune
 STR14797: strong ultimate healing rune
 STR14800: ultimate healing rune
 STR14803: intense healing rune
 STR14806: AntiPara:
 STR14809: AutoHur:
 STR15107: Low Health:
 STR15210: Low Health.wav
 STR15448: Light Healing
 STR15449: Enter health percent your character. Priority=3
 STR15455: Enter spell, id or shortcut e.g uhp, shp, gsp, suhp to heal your character.
 STR15458: Enter spell or id to heal your character.
 STR15460: Medium Healing
 STR15461: Enter health percent your character. Priority=2
 STR15472: Heavy Healing
 STR15473: Enter health percent your character. Priority=1
 STR15485: Enter health percent to use runes. Priority=1
 STR15488: Heal your character with Ultimate Healing Rune. Priority=1
 STR15489: Leczenie postaci za pomoc� Ultimate Healing Rune. Priorytet=1.
 STR15509: Enter health percent to drink fluids. Priority=3
 STR15512: Restore health using life fluids. Priority=3
 STR15515: Enter friends health percent to start action.
Funções relacionadas (nome ofuscado, linhas, quantidade de strings):
 HU L54175-L54271 strings=84
<PARSED TEXT FOR PAGE: 28 / 88>
 JA L54879-L54897 strings=7
 JB L55113-L55221 strings=94
 JM L55274-L55323 strings=12
 J1 L55358-L55403 strings=14
 J2 L55404-L55455 strings=88
 J3 L55456-L55586 strings=87
 KZ L56643-L56674 strings=27
 LW L56963-L56971 strings=1
 LT L57064-L57291 strings=65
 LD L57516-L57677 strings=379
 LB L57814-L57881 strings=52
 L2 L58028-L58109 strings=33
 XF L59377-L59468 strings=40
 XK L59497-L59575 strings=37
 VV L64590-L64901 strings=195
 BA L65025-L65190 strings=84
 WEW L66623-L66833 strings=22
Lua / Scripting API
249 strings relacionadas; 87 funções com referências relacionadas.
 STR71: WinDescription
 STR73: Description
 STR650: lua5.1.dll
 STR4663: lua_call
 STR4668: lua_pcall
 STR4674: lua_close
 STR4677: lua_getfield
 STR4687: lua51-x64.dll
 STR4690: luaL_newstate
 STR4692: luaL_openlibs
 STR4695: luaopen_base
 STR4697: luaopen_table
 STR4699: luaopen_io
 STR4701: luaopen_os
 STR4703: luaopen_string
 STR4705: luaopen_math
 STR4707: luaopen_debug
 STR4713: lua_settop
 STR4719: lua_pushcclosure
 STR4724: luaL_loadfile
 STR4738: luaL_loadbuffer
 STR4745: lua_setfield
 STR4750: lua_toboolean
 STR4754: lua_tointeger
 STR4758: lua_tonumber
 STR4762: lua_tolstring
 STR4767: lua_createtable
<PARSED TEXT FOR PAGE: 29 / 88>
 STR4772: lua_settable
 STR4776: lua_gettable
 STR4780: lua_rawget
 STR4784: lua_rawgeti
 STR4789: lua_rawset
 STR4793: lua_gettop
 STR4800: lua_pushboolean
 STR4804: lua_pushnumber
 STR4808: lua_pushlstring
 STR4813: lua_pushstring
 STR4817: lua_pushnil
 STR4820: lua_pushinteger
 STR4824: lua_pushvalue
 STR4828: lua_pushthread
 STR4831: lua_tothread
 STR4835: lua_isnil
 STR4839: lua_isboolean
 STR4843: lua_isnumber
 STR4847: lua_isstring
 STR4851: lua_istable
 STR4855: lua_isfunction
 STR4859: lua_iscfunction
 STR4863: lua_isuserdata
 STR4867: lua_islightuserdata
 STR4871: lua_error
 STR4874: lua_insert
 STR4877: lua_type
 STR4881: lua_objlen
 STR4885: lua_newthread
 STR4888: luaL_ref
 STR4892: luaL_unref
 STR4897: lua_setmetatable
 STR4901: lua_replace
 STR4905: lua_pushliteral
 STR4909: lua_resume
 STR4913: lua_xmove
 STR4918: lua_yield
 STR6237: If appear lua lib error then put files: lua51.dll, lua5.1.dll and Rifbot Lib.lua to loaction C:\Users\You
 STR6243: Je�eli wyst�pi b��d o bibliotece lua to wrzu� pliki: lua51.dll, lua5.1.dll oraz Rifbot Lib.lua do lokacji
C:\Users\Twoja Nazwa
 STR6266: A: Use more lua scripts with your custom timers to randomize actions.
 STR6267: Q: Do yo have lua script for..?
 STR6268: A: All my lua script you can find on GitHub: https://github.com/Ascerr/Lua-Scripts
 STR6291: A: U�ywaj wi�cej lua script�w z w�asnym losowym czasem powtarzania czynno�ci.
 STR6292: Q: Masz mo�e lua script na ..?
 STR6293: A: Wszystkie lua scripty napisane przeze mnie znajdziesz tutaj: https://github.com/Ascerr/Lua-Scripts
 STR6309: Stopping functions without alarms and lua scripts. To enable it again press Ctrl+P.
 STR6314: Lua Scripts {Ctrl+L}
 STR6315: Opens panel with lua scripts. More info in section Lua Scripts.
<PARSED TEXT FOR PAGE: 30 / 88>
 STR6324: Zatrzymuje funkcje bota poza Alarmami oraz scriptami Lua. Aby ponownie go uruchomi� wci�nij 
Ctrl+P.
 STR6330: Otwiera panel z Lua scriptami. Wiecej informacji w sekcji Lua Scripts.
 STR6508: [Only for Retrocores/Mastercores] If we are under antibot test then Rifbot automatically stops all 
functions and LUA scripts even is this function is disabled. If function is enabled then will play loud sound.
 STR6523: [Tylko na Retrocores/Mastercores] Je�eli b�dziemy w trakcie testu przeciwko botom to Rifbot 
automatycznie zatrzyma wszystkie funkcje w panelu oraz scripty lua nawet jak ten kwadracik jest nie aktywny. 
Je�eli jest aktywny to b�dzie wydobywa� si� g�o�ny d�wi�k.
 STR6685: EXECUTE scriptName
Funções relacionadas (nome ofuscado, linhas, quantidade de strings):
 QS L17934-L17938 strings=21
 RU L19238-L19261 strings=7
 TU L23483-L23486 strings=5
 TI L23487-L23492 strings=6
 TP L23493-L23501 strings=3
 TA L23502-L23505 strings=5
 TS L23506-L23509 strings=5
 TD L23510-L23522 strings=5
 TF L23523-L23526 strings=3
 TG L23527-L23549 strings=18
 TH L23550-L23553 strings=4
 TL L23579-L23582 strings=5
 TZ L23583-L23588 strings=10
 TX L23589-L23594 strings=4
 TC L23595-L23600 strings=7
 TV L23601-L23604 strings=5
 TB L23605-L23613 strings=4
 TN L23614-L23618 strings=4
 TM L23619-L23623 strings=4
 T_ L23624-L23628 strings=5
 T1 L23634-L23637 strings=5
 T3 L23642-L23645 strings=4
 T4 L23646-L23650 strings=4
 T5 L23651-L23655 strings=4
 T6 L23656-L23660 strings=5
 T7 L23661-L23664 strings=4
 YQ L23665-L23669 strings=3
 YW L23670-L23674 strings=4
 YE L23675-L23678 strings=4
 YR L23679-L23682 strings=4
 YT L23683-L23686 strings=5
 YY L23687-L23690 strings=4
 YU L23691-L23694 strings=3
 YI L23695-L23698 strings=4
 YO L23699-L23702 strings=4
 YP L23703-L23706 strings=3
 YA L23707-L23711 strings=4
<PARSED TEXT FOR PAGE: 31 / 88>
 YS L23712-L23716 strings=4
 YD L23717-L23721 strings=4
 YF L23722-L23726 strings=4
 YG L23727-L23731 strings=4
 YH L23732-L23736 strings=4
 YJ L23737-L23741 strings=4
 YK L23742-L23746 strings=4
 YL L23747-L23751 strings=4
 YZ L23752-L23756 strings=4
 YX L23757-L23761 strings=3
 YC L23762-L23765 strings=3
 YV L23766-L23770 strings=4
 YB L23771-L23775 strings=4
 YN L23776-L23780 strings=3
 YM L23781-L23785 strings=4
 Y_ L23786-L23790 strings=5
 Y0 L23791-L23795 strings=4
 Y1 L23796-L23800 strings=4
 Y2 L23801-L23805 strings=4
 Y3 L23806-L23810 strings=4
 Y4 L23811-L23815 strings=5
 Y5 L23816-L23820 strings=4
 GN L53442-L53701 strings=12
 G3 L53766-L53811 strings=11
 HU L54175-L54271 strings=84
 HI L54272-L54277 strings=4
 KN L56754-L56805 strings=24
 LI L57379-L57436 strings=35
 LV L57774-L57813 strings=35
 LB L57814-L57881 strings=52
 L1 L57923-L58027 strings=42
 ZS L58358-L58370 strings=4
 ZJ L58456-L58682 strings=192
 X0 L59739-L62394 strings=304
 VA L64299-L64349 strings=19
 VF L64365-L64377 strings=2
 VG L64378-L64414 strings=3
 VJ L64466-L64490 strings=1
 VK L64491-L64504 strings=1
 VZ L64511-L64518 strings=4
 VX L64519-L64522 strings=1
 VC L64523-L64589 strings=28
 VV L64590-L64901 strings=195
 VB L64902-L64925 strings=20
 BX L65245-L65250 strings=1
 NR L65405-L65444 strings=6
 WWX L66368-L66389 strings=3
 WWC L66390-L66427 strings=8
 WWV L66428-L66465 strings=5
<PARSED TEXT FOR PAGE: 32 / 88>
 WWB L66466-L66499 strings=7
Input / UI
15 strings relacionadas; 5 funções com referências relacionadas.
 STR232: GetAsyncKeyState
 STR10641: {"processesRunning":
{"TfmSecond":false,"TinyTaskClass":false,"AutoHotkeyGUI":false,"KasteriaOTBot":false,"MiniMouseMacro":false,"
AutoItv3GUI":false,"FMTmrMainFMX:false"}}
Funções relacionadas (nome ofuscado, linhas, quantidade de strings):
 Q5 L18265-L18269 strings=5
 Q6 L18270-L18274 strings=5
 EH L18958-L18962 strings=4
 S5 L44664-L44700 strings=31
 LY L57292-L57352 strings=45
Network / Protocol
29 strings relacionadas; 9 funções com referências relacionadas.
 STR89: SendMessageW
 STR6246: A: I send key manually to your email within 24h after purchase.
 STR6258: A: Rifbot can't reach license server. Check your internet connection. If all is ok send me email.
 STR6464: Relogin After
 STR6465: Character login to game after some time e.g. 6 min.
 STR6510: [Only for Retrocores/Mastercores] Character make logout due antibot test. Warring! after login you will 
have (~2min) to enter code.
 STR6598: Activate sending keys [F1-F12 + Nums] to all game clients.
 STR13982: Sockets are bussy..
 STR14978: Relogin After:
 STR15031: Relogin
 STR15033: ] Successfully relogin.
 STR15048: Relogin is
 STR15084: ] You has been logout due the internet or game issue. Relogin is disabled.
 STR15091: ] You has been logout due the internet or game issue. Relogin is enabled
 STR15097: Relogin for
 STR15164: send you an private message. Options -> Open Proxy
 STR15641: [Only for Retrocores server] Logout your character due macro test. !WARRING after login you will 
have small amount of time to pass test.
 STR15659: Relogin to game. Enter time [minutes] to relogin. If 0 then login after ~5s.
 STR15662: Relogin to game while kicked or after player logout. Runemaker need to be enabled.
 STR15668: Logout your character when out of blank runes or soul points. Read proxy, works even with closed 
backpacks. Use easy with player logout or relogin.
 STR16219: Walker: Login to game to add waypoint.
 STR17370: httpSend
 STR17435: relogin after
 STR17764: If you don't find answear in tutorial send me email:
<PARSED TEXT FOR PAGE: 33 / 88>
Funções relacionadas (nome ofuscado, linhas, quantidade de strings):
 QD L17984-L17989 strings=7
 KD L56179-L56222 strings=37
 KG L56300-L56509 strings=76
 KN L56754-L56805 strings=24
 LD L57516-L57677 strings=379
 ZJ L58456-L58682 strings=192
 VP L64293-L64298 strings=7
 VV L64590-L64901 strings=195
 BA L65025-L65190 strings=84
13. Inventário das funções recuperadas
Este apêndice lista as 962 funções recuperadas. Os nomes são os nomes ofuscados do payload; a coluna de strings 
ajuda a atribuir semântica.
QQ [L17810-L17812] ( $0W )
QW [L17813-L17823] ( )
uint;dword | user32.dll | bool | GetLastInputInfo | struct* | kernel32.dll | dword | GetTickCount
QE [L17824-L17830] ( $0U )
QR [L17831-L17833] ( )
QT [L17834-L17851] ( $0A )
user32.dll | bool | KillTimer | hwnd | uint_ptr | user32.dll | bool | KillTimer
QY [L17852-L17875] ( $0A , $0F )
user32.dll | bool | KillTimer | hwnd | uint_ptr | user32.dll | bool | KillTimer
QU [L17876-L17880] ( )
kernel32.dll | bool | QueryPerformanceCounter | int64*
QI [L17881-L17885] ( )
kernel32.dll | bool | QueryPerformanceFrequency | int64*
QO [L17886-L17925] ( $0A , $0J = 250 , $0K = "" , $0F = + 4294967295 )
none | hwnd;uint;uint_ptr;dword | user32.dll | uint_ptr | SetTimer | hwnd | uint_ptr | uint
QP [L17926-L17929] ( CONST $0X = @ERROR , CONST $0C = @EXTENDED )
kernel32.dll | dword | GetLastError
QA [L17930-L17933] ( $0V , CONST $0X = @ERROR , CONST $0C = @EXTENDED )
<PARSED TEXT FOR PAGE: 34 / 88>
kernel32.dll | none | SetLastError | dword
QS [L17934-L17938] ( $0B , $0N = @TAB )
COM Error encountered in | ( | ) : | Number | = 0x | ( | ) | WinDescription
QD [L17984-L17989] ( $0A , $2E , $0U = 0 , $2R = 0 , $2T = 0 , $2Y = $STR85 , $2U = $STR86 , $2I = $STR87 )
wparam | lparam | lresult | user32.dll | SendMessageW | hwnd | uint
QF [L17992-L17996] ( $0A , $2A , $2S = TRUE )
user32.dll | bool | ShowScrollBar | hwnd | int | bool
QG [L18053-L18063] ( $4W , $4E = 0 , $4R = FALSE )
QH [L18064-L18068] ( $4U = $34 )
advapi32.dll | bool | ImpersonateSelf | int
QJ [L18069-L18078] ( $4W , $4E = 0 , $4R = FALSE )
kernel32.dll | handle | GetCurrentThread | advapi32.dll | bool | OpenThreadToken | handle | dword
QK [L18079-L18102] ( $4T , $4O , $4P )
dword Count;align 4;int64 LUID;dword Attributes | int Data | Count | LUID | Count | LUID | Attributes | Attributes
QL [L18103-L18107] ( $4L , $4Z )
advapi32.dll | bool | LookupPrivilegeValueW | wstr | wstr | int64*
QZ [L18108-L18112] ( $4T , $4X , $4C , $4V , $4G = 0 , $4B = 0 )
advapi32.dll | bool | AdjustTokenPrivileges | handle | bool | struct* | dword | struct*
QX [L18126-L18141] ( $0A , $5E , BYREF $5R )
user32.dll | dword | GetWindowThreadProcessId | hwnd | dword* | hProc | Size | Mem
QC [L18142-L18146] ( $5Y , $5O , $5E , $5P , $5A )
kernel32.dll | ptr | VirtualAllocEx | handle | ptr | ulong_ptr | dword | dword
QV [L18147-L18173] ( $4W , $5S , $5T , $5D = FALSE )
kernel32.dll | handle | OpenProcess | dword | bool | dword | SeDebugPrivilege | kernel32.dll
QB [L18174-L18180] ( BYREF $5R , $5J , $5K = 0 , $5E = 0 , $5L = $STR177 )
struct* | Mem | Size | kernel32.dll | bool | WriteProcessMemory | handle | hProc
QN [L18181-L18185] ( BYREF $5R , $5J , $5K , $5E )
kernel32.dll | bool | ReadProcessMemory | handle | hProc | ptr | struct* | ulong_ptr
QM [L18186-L18193] ( BYREF $5R )
<PARSED TEXT FOR PAGE: 35 / 88>
Mem | hProc | kernel32.dll | bool | CloseHandle | handle
Q_ [L18194-L18198] ( $5Y , $5O , $5E , $5X )
kernel32.dll | bool | VirtualFreeEx | handle | ptr | ulong_ptr | dword
Q0 [L18203-L18217] ( $5N , $5M , $5_ , $50 = $5C , $51 = FALSE )
\Q | \E | \A | (?=\Q | \E) | \z | \Q | \E
Q1 [L18218-L18228] ( $53 )
Q2 [L18229-L18236] ( $55 )
Q3 [L18237-L18243] ( $57 )
\R
Q4 [L18244-L18264] ( $57 , $6E = $STR224 , $6R = $15 , $6T = FALSE )
* | | | [\\/]+$ | \ | * | [\\/:><\|]|(?s)^\s*$
Q5 [L18265-L18269] ( $6A , $6S = $STR230 )
user32.dll | short | GetAsyncKeyState | int | 0x
Q6 [L18270-L18274] ( $6F )
user32.dll | short | GetAsyncKeyState | int | 0x
Q7 [L18299-L18302] ( $0A , $7T )
WQ [L18303-L18306] ( $0A , $7Y , $7U )
WW [L18307-L18314] ( $0A , $7I , $7O )
WE [L18315-L18318] ( $0A , $0Z = + 4294967295 )
WR [L18319-L18323] ( $0A , $0Z = + 4294967295 )
WT [L18324-L18334] ( $0A , $7A )
short Len;wchar Text[ | ] | Len | wparam | struct* | wchar Text[ | ] | Text
WY [L18335-L18343] ( $0A , $7G )
WU [L18344-L18360] ( $0A , $7H )
int; | wparam | struct*
WI [L18387-L18389] ( $0A )
WO [L18390-L18396] ( $0A )
WP [L18397-L18403] ( $0A )
WA [L18404-L18411] ( $0A , $8A = + 4294967295 )
<PARSED TEXT FOR PAGE: 36 / 88>
GUIDataSeparatorChar
WS [L18412-L18423] ( $0A , $8A = + 4294967295 )
GUIDataSeparatorChar
WD [L18424-L18436] ( $0A , $7Y = + 4294967295 , $8J = 0 , $8K = 8 )
WF [L18437-L18478] ( $0A , $0Z , $8X = 0 )
wchar Text[4096] | char Text[4096] | SubItem | TextMax | Text | wparam | struct* | Text
WG [L18479-L18485] ( $0A )
WH [L18486-L18493] ( $0A , $0Z , $8X = 0 )
Mask | Item | SubItem | Image
WJ [L18494-L18522] ( $0A , BYREF $8B )
wparam | struct* | wparam | ptr | wparam | ptr
WK [L18523-L18580] ( $0A , $0Z , $8_ , $8X = 0 )
GUIDataSeparatorChar | wchar Text[ | ] | char Text[ | ] | Text | Mask | item
WL [L18581-L18588] ( $0A , $0Z , $83 , $8X = 0 )
Mask | Item | SubItem | Image
WZ [L18589-L18618] ( $0A , BYREF $8B )
TextMax | Text | Text | wparam | ptr | wparam | ptr
WX [L18619-L18625] ( $0A , $84 , $85 )
WC [L18626-L18646] ( $0A )
WV [L18647-L18653] ( $0A , $0Z )
Mask | Item | Param
WB [L18654-L18682] ( $0A , $9Q = FALSE )
|
WN [L18683-L18700] ( $0A , $0Z )
WM [L18701-L18749] ( $0A , $8_ , $0Z = + 4294967295 , $83 = + 4294967295 , $87 = 0 )
Param | wchar Text[ | ] | char Text[ | ] | Text | Text | TextMax
W_ [L18750-L18752] ( $0A , $8_ , $83 = + 4294967295 , $87 = 0 )
W0 [L18753-L18755] ( )
: | :
<PARSED TEXT FOR PAGE: 37 / 88>
W1 [L18756-L18762] ( $9Y = $1S )
user32.dll | handle | CreatePopupMenu
W2 [L18763-L18768] ( $9U , BYREF $9I )
Size | user32.dll | bool | SetMenuInfo | handle | struct*
W3 [L18769-L18774] ( $9U , $9Y )
Mask | Style
W4 [L18775-L18794] ( $9U , $8_ , $9O = 0 , $9P = 0 )
Size | ID | SubMenu | Mask | Type | Mask | Type | wchar Text[
W5 [L18795-L18799] ( $9U )
user32.dll | int | GetMenuItemCount | handle
W6 [L18800-L18831] ( $9U , $0A , $7I = + 4294967295 , $7O = + 4294967295 , $9S = 1 , $9D = 1 , $9F = 0 , $9G = 0 )
user32.dll | bool | TrackPopupMenu | handle | uint | int | int | int
W7 [L18832-L18836] ( $0A , $9U )
user32.dll | bool | SetMenu | hwnd | handle
EQ [L18837-L18855] ( $9U , $0Z , $8_ , $9O = 0 , $9P = 0 )
Size | ID | SubMenu | Mask | Type | Mask | Type | wchar Text[
EW [L18856-L18862] ( $9Y = $1S )
user32.dll | handle | CreateMenu
EE [L18863-L18867] ( $0A )
user32.dll | handle | GetMenu | hwnd
ER [L18879-L18881] ( $9_ , $90 )
ET [L18882-L18884] ( $91 )
EY [L18885-L18887] ( $91 )
EU [L18888-L18908] ( $0A , BYREF $92 )
EI [L18909-L18914] ( $0A , BYREF $53 )
user32.dll | dword | GetWindowThreadProcessId | hwnd | dword*
EO [L18915-L18919] ( $0A )
user32.dll | int | GetDlgCtrlID | hwnd
EP [L18920-L18924] ( $94 = FALSE , $0A = 0 )
<PARSED TEXT FOR PAGE: 38 / 88>
X
EA [L18925-L18929] ( $94 = FALSE , $0A = 0 )
Y
ES [L18930-L18939] ( $94 = FALSE , $0A = 0 )
MouseCoordMode | MouseCoordMode | X | Y
ED [L18940-L18944] ( $0A , BYREF $95 )
user32.dll | bool | ScreenToClient | hwnd | struct*
EF [L18945-L18950] ( $0A )
user32.dll | int | GetClassNameW | hwnd | wstr | int
EG [L18951-L18957] ( $0A )
user32.dll | int | GetClassNameW | hwnd | wstr | int
EH [L18958-L18962] ( $97 )
user32.dll | short | GetAsyncKeyState | int
EJ [L18963-L18967] ( $0A , BYREF $95 )
user32.dll | bool | ClientToScreen | hwnd | struct*
EK [L18968-L18975] ( $0A , $0Z , $10W )
SetWindowLongW | SetWindowLongPtrW | user32.dll | long_ptr | hwnd | int | long_ptr
EL [L18976-L18982] ( $0A )
length | user32.dll | bool | GetWindowPlacement | hwnd | struct*
EZ [L18983-L18988] ( $0A , $10T = 0 , $10Y = TRUE )
User32.dll | int | InvalidateRect | hwnd | ptr | int
EX [L19001-L19011] ( $10Z , $10X = 2035711 , $10C = 1 )
kernel32.dll | int | OpenProcess | int | int | int
EC [L19012-L19016] ( $10M , $10N , $10_ = $STR474 )
dword_ptr | bool | ReadProcessMemory | handle | ptr | ptr | ulong_ptr | ulong_ptr*
EV [L19017-L19022] ( $10M , $10N , $101 , $10_ = $STR482 )
dword_ptr | bool | WriteProcessMemory | handle | ptr | ptr | ulong_ptr | ulong_ptr*
EB [L19023-L19028] ( $10M , $10N , $103 )
byte[ | ] | bool | WriteProcessMemory | handle | ptr | ptr | int
<PARSED TEXT FOR PAGE: 39 / 88>
EN [L19029-L19034] ( $10M , $10N , $104 )
byte[ | ] | 0x | bool | WriteProcessMemory | handle | ptr | ptr
EM [L19035-L19041] ( $10N )
int | CloseHandle | int
E_ [L19042-L19064] ( $53 , $5Y , $106 )
int | uint64 | psapi.dll | ptr[1024] | EnumProcessModules | ptr | ptr | dword
E0 [L19065-L19070] ( $11Y , $11U , $11I , $11O )
byte | char[ | ] | char[ | ]
E1 [L19071-L19075] ( $11Y , $11U , $11S )
ushort
E2 [L19076-L19084] ( $11A , $11S , $11F = 8 )
0x
E3 [L19085-L19091] ( $10N , $11H = 4096 )
int | uint64 | VirtualAllocEx | int | short | int | int | int
E4 [L19092-L19096] ( $5O , $10N )
int | uint64 | kernel32.dll | VirtualFreeEx | handle | ptr | ulong_ptr | dword
E5 [L19097-L19103] ( $5O , $10N , $4W = 64 , $5E = 1024 )
dword | kernel32.dll | bool | VirtualProtectEx | handle | ptr | ulong_ptr | dword
E6 [L19104-L19142] ( $11L = $STR558 , $11Z = 1 )
sedebugprivilege | int | uint64 | dword;int | dword;dword[ | ] | dword;dword[ | ]
E7 [L19143-L19152] ( $115 )
int | uint64 | kernel32.dll | OpenProcess | int | int | int | psapi.dll
RQ [L19153-L19173] ( $53 , $5Y , $106 )
int | uint64 | psapi.dll | ptr[1024] | EnumProcessModules | ptr | ptr | dword
RW [L19174-L19199] ( $10M , $10N )
ptr;ptr;dword;dword;uint64;dword;dword;dword;dword | dword;dword;dword;dword;dword;dword;dword;dword 
| ptr | VirtualQueryEx | handle | ptr | ptr | int
RE [L19200-L19212] ( )
kernel32.dll | ptr | VirtualAlloc | ptr | ulong_ptr | dword | dword | byte[
<PARSED TEXT FOR PAGE: 40 / 88>
RR [L19213-L19216] ( $12W , $12R = $STR637 )
dword_ptr
RT [L19217-L19220] ( $12W , $12T )
byte[ | ] | 0x
RY [L19221-L19237] ( )
C:\ | .exe | node: | 0000 | 0x | 0x
RU [L19238-L19261] ( )
ushort | wchar[ | ] | lua5.1.dll | C:\msvcrt.dll | 0x | 0000
RI [L19262-L19280] ( $11G , $11H = 8 , $12R = $STR654 )
dword | dword | 00000000 | 00 | dword
RO [L19281-L19290] ( $12Z , $12X )
RP [L19291-L19320] ( BYREF $12C , $12V , $12B = NULL )
RA [L19321-L19341] ( BYREF $12C , $12V , $121 = FALSE )
RS [L19342-L19382] ( $124 )
Array | Variable is not array. Variable = | Ubound: | [ | ] - | [ | ] - | [
RD [L19383-L19390] ( $12C )
RF [L19391-L19398] ( $13S )
RG [L19399-L19409] ( $11A , $13F , $13G = FALSE )
RH [L19410-L19425] ( $11A )
 |
RJ [L19426-L19432] ( $11A , $13K = 1 )
RK [L19433-L19444] ( $13L )
RL [L19445-L19481] ( $13V , $11U = NULL , $13B = FALSE )
shift | ctrl | A3 | A2 | ctrl + f | 0x | A0 | A1
RZ [L19482-L19492] ( $6F , $11U , $132 = 200 )
user32.dll | int | MapVirtualKey | int | int | user32.dll | int | PostMessage
RX [L19493-L19504] ( $134 , $135 , $11U , $132 = 0 )
user32.dll | int | MapVirtualKey | int | int | user32.dll | int | MapVirtualKey
RC [L19505-L19507] ( $14E , $14R )
<PARSED TEXT FOR PAGE: 41 / 88>
RV [L19508-L19510] ( $14E , $14R )
RB [L19511-L19513] ( $0P , $14T )
RN [L19514-L19538] ( $0P , $14T , $11U , $14Y = FALSE , $132 = 0 , $14U = FALSE )
left | user32.dll | int | PostMessage | hwnd | uint | wparam | lparam
RM [L19539-L19567] ( $14S , $14D , $14F , $14G , $11U , $14U = FALSE )
user32.dll | int | MapVirtualKey | int | int | {CTRLDOWN} | left | {CTRLUP}
R_ [L19568-L19574] ( $13I , $11U )
user32.dll | int | PostMessage | hwnd | int | int | long
R0 [L19575-L19577] ( $12T )
R1 [L19578-L19592] ( $14K , $14L = DEFAULT , $14Z = DEFAULT , $14X = DEFAULT , $14C = DEFAULT , $14V = 
DEFAULT , $14B = DEFAULT , $14N = DEFAULT , $14M = DEFAULT , $14_ = DEFAULT , $140 = DEFAULT , $1)
, | , | , | , | , | , | , | ,
R2 [L19593-L19600] ( )
ALL | #
L [L19601-L19603] ( $11A , $105 = @ERROR )
R3 [L19604-L19609] ( $0A )
R4 [L19610-L19614] ( $145 = 1 )
winmm.dll | dword | timeBeginPeriod | int | kernel32.dll | none | Sleep | int
R5 [L19615-L19621] ( $55 )
R6 [L19622-L19641] ( $147 )
kernel32.dll | handle | CreateToolhelp32Snapshot | dword | dword | dword dwSize; | dword cntUsage; | dword 
th32ProcessID;
R7 [L19642-L19644] ( $15T )
TQ [L19645-L19648] ( $15Y = "" )
setTime:
TW [L19649-L19651] ( )
getTimeDiff: | ms
TE [L19652-L19677] ( $0A )
flags | showCmd | rcNormalPosition | rcNormalPosition | rcNormalPosition | rcNormalPosition
TR [L19678-L19687] ( $121 = 0 )
<PARSED TEXT FOR PAGE: 42 / 88>
winmgmts:\\.\root\cimv2 | SELECT * FROM Win32_PhysicalMedia Where Tag like '%PHYSICALDRIVE | ' | ^\h*(.*?)\h* 
| $1
TT [L19688-L19694] ( $15T )
TY [L19695-L19697] ( $15D )
disabled
TU [L23483-L23486] ( $16T , $16Y , $5H )
none:cdecl | lua_call | handle | int | int
TI [L23487-L23492] ( $16T , $16Y , $5H , $16U = 0 )
int:cdecl | lua_pcall | handle | int | int | int
TP [L23493-L23501] ( $16I )
none:cdecl | lua_close | handle
TA [L23502-L23505] ( BYREF $16I , $4Z )
none:cdecl | lua_getfield | handle | int | str
TS [L23506-L23509] ( BYREF $16I , $121 , $4Z )
none:cdecl | lua_getfield | handle | int | str
TD [L23510-L23522] ( )
lua5.1.dll | lua51-x64.dll | \ | handle:cdecl | luaL_newstate
TF [L23523-L23526] ( BYREF $16I )
none:cdecl | luaL_openlibs | handle
TG [L23527-L23549] ( BYREF $16I , $16P )
coroutine | luaopen_base | table | luaopen_table | io | luaopen_io | os | luaopen_os
TH [L23550-L23553] ( BYREF $16I , $0Z )
none:cdecl | lua_settop | handle | int
TJ [L23554-L23570] ( $16A )
int:cdecl | handle
TK [L23571-L23578] ( BYREF $16I , $16A , $16D )
TL [L23579-L23582] ( BYREF $16I , $16F , $16G = 0 )
none:cdecl | lua_pushcclosure | handle | handle | int
TZ [L23583-L23588] ( $16T , $6I )
<PARSED TEXT FOR PAGE: 43 / 88>
int:cdecl | luaL_loadfile | handle | str | int:cdecl | lua_pcall | handle | int
TX [L23589-L23594] ( $16T , $6I )
int:cdecl | luaL_loadfile | handle | str
TC [L23595-L23600] ( $16T , $11A )
int:cdecl | luaL_loadbuffer | handle | str | int | str | line
TV [L23601-L23604] ( BYREF $16I , $4Z )
none:cdecl | lua_setfield | handle | int | str
TB [L23605-L23613] ( BYREF $16I , $0Z )
boolean:cdecl | lua_toboolean | handle | int
TN [L23614-L23618] ( BYREF $16I , $0Z )
int_ptr:cdecl | lua_tointeger | handle | int
TM [L23619-L23623] ( BYREF $16I , $0Z )
double:cdecl | lua_tonumber | handle | int
T_ [L23624-L23628] ( BYREF $16I , $0Z , $16H = 0 )
str:cdecl | lua_tolstring | handle | int | int
T0 [L23629-L23633] ( BYREF $16I , $16J , $16D )
T1 [L23634-L23637] ( $16T , $16K = 0 , $16L = 0 )
none:cdecl | lua_createtable | handle | int | int
T2 [L23638-L23641] ( $16T )
T3 [L23642-L23645] ( $16T , $0Z = + 4294967293 )
none:cdecl | lua_settable | handle | int
T4 [L23646-L23650] ( BYREF $16I , $0Z )
none:cdecl | lua_gettable | handle | int
T5 [L23651-L23655] ( BYREF $16I , $0Z )
double:cdecl | lua_rawget | handle | int
T6 [L23656-L23660] ( BYREF $16I , $0Z = + 4294967293 , $16Z = 0 )
double:cdecl | lua_rawgeti | handle | int | int
T7 [L23661-L23664] ( BYREF $16I , $0Z = + 4294967293 )
none:cdecl | lua_rawset | handle | int
<PARSED TEXT FOR PAGE: 44 / 88>
YQ [L23665-L23669] ( BYREF $16I )
int:cdecl | lua_gettop | handle
YW [L23670-L23674] ( BYREF $16I , $121 )
none:cdecl | lua_settop | handle | int
YE [L23675-L23678] ( BYREF $16I , $11G )
none:cdecl | lua_pushboolean | handle | bool
YR [L23679-L23682] ( BYREF $16I , $11G )
none:cdecl | lua_pushnumber | handle | double
YT [L23683-L23686] ( $16T , $11G , $12O = 0 )
none:cdecl | lua_pushlstring | handle | str | int
YY [L23687-L23690] ( $16T , $11G )
none:cdecl | lua_pushstring | handle | str
YU [L23691-L23694] ( BYREF $16I )
none:cdecl | lua_pushnil | handle
YI [L23695-L23698] ( BYREF $16T , $11G )
none:cdecl | lua_pushinteger | handle | int_ptr
YO [L23699-L23702] ( $16T , $11G )
none:cdecl | lua_pushvalue | handle | int
YP [L23703-L23706] ( $16T )
none:cdecl | lua_pushthread | handle
YA [L23707-L23711] ( $16T , $121 )
ptr:cdecl | lua_tothread | handle | int
YS [L23712-L23716] ( BYREF $16I , $121 )
int:cdecl | lua_isnil | handle | int
YD [L23717-L23721] ( BYREF $16I , $121 )
int:cdecl | lua_isboolean | handle | int
YF [L23722-L23726] ( BYREF $16I , $121 )
int:cdecl | lua_isnumber | handle | int
YG [L23727-L23731] ( BYREF $16I , $121 )
<PARSED TEXT FOR PAGE: 45 / 88>
int:cdecl | lua_isstring | handle | int
YH [L23732-L23736] ( BYREF $16I , $121 )
int:cdecl | lua_istable | handle | int
YJ [L23737-L23741] ( BYREF $16I , $121 )
int:cdecl | lua_isfunction | handle | int
YK [L23742-L23746] ( BYREF $16I , $121 )
int:cdecl | lua_iscfunction | handle | int
YL [L23747-L23751] ( BYREF $16I , $121 )
int:cdecl | lua_isuserdata | handle | int
YZ [L23752-L23756] ( BYREF $16I , $121 )
int:cdecl | lua_islightuserdata | handle | int
YX [L23757-L23761] ( BYREF $16I )
int:cdecl | lua_error | handle
YC [L23762-L23765] ( BYREF $16I , $11G )
none:cdecl | lua_insert | handle
YV [L23766-L23770] ( BYREF $16I , $121 )
int:cdecl | lua_type | handle | int
YB [L23771-L23775] ( BYREF $16I , $121 )
int:cdecl | lua_objlen | handle | int
YN [L23776-L23780] ( $16T )
handle:cdecl | lua_newthread | handle
YM [L23781-L23785] ( BYREF $16I , $121 )
int:cdecl | luaL_ref | handle | int
Y_ [L23786-L23790] ( BYREF $16I , $121 , $16X )
none:cdecl | luaL_unref | handle | int | int
Y0 [L23791-L23795] ( $16T , $121 )
none:cdecl | lua_setmetatable | handle | int
Y1 [L23796-L23800] ( $16T , $121 )
none:cdecl | lua_replace | handle | int
<PARSED TEXT FOR PAGE: 46 / 88>
Y2 [L23801-L23805] ( $16T , $13H )
none:cdecl | lua_pushliteral | handle | str
Y3 [L23806-L23810] ( $16T , $16C )
int:cdecl | lua_resume | handle | int
Y4 [L23811-L23815] ( BYREF $16T , BYREF $16V , $16Z )
int:cdecl | lua_xmove | handle | handle | int
Y5 [L23816-L23820] ( $16T , $16B )
int:cdecl | lua_yield | handle | int
Y6 [L24283-L24297] ( )
[REGEXPCLASS:(?i)( | )] | medivia
Y7 [L24298-L24341] ( )
-c | -c | .exe | " | \ | \ | 0415 | Error CMD 01
UQ [L36267-L36277] ( $37W , $37E , $37R )
UW [L36389-L36606] ( )
.exe | \ | B��d 11 | Plik | nie istnieje, pobierz ponownie bota. | Error 11 | File | does not exist, re-download bot.
UE [L36608-L36633] ( $43U , $43I = NULL )
byte[ | ] | 0x | uint64 | WriteProcessMemory | int | ptr | ptr
UR [L36634-L36636] ( )
UT [L36637-L36643] ( $11F )
90 | 90
UY [L36644-L36646] ( $12W )
B8 | FFD0
UU [L36647-L36649] ( $12W , $11G )
C705
UI [L36650-L36652] ( $11G )
B9
UO [L36653-L36655] ( )
C3
UP [L36656-L36659] ( $125 )
<PARSED TEXT FOR PAGE: 47 / 88>
8B | 0D | 00000000
UA [L36660-L36662] ( $11G )
83C4
US [L36663-L36679] ( $11G , $12W , $43F = $STR7163 )
68 | B9 | 6A | B1 | B8 | FFD0 | B8 | B8
UD [L36680-L36688] ( $11G , $12W , $12R = $STR7173 )
ecx | B9 | cl | B1 | [ecx] | 8B0D | B8 | FFD0
UF [L36689-L36701] ( $11G , $12W , $43F = $STR7181 )
68 | <not implemented> | 8B4DEC | 68 | 68 | 6A | B8 | FFD0
UG [L36702-L36712] ( )
UH [L36713-L36715] ( )
UJ [L36716-L36718] ( )
UK [L36719-L36721] ( )
UL [L36722-L36724] ( )
UZ [L36725-L36727] ( )
UX [L36728-L36793] ( )
char[36] | char[36] | 83 | manaFluidItem.count = | FF
UC [L36794-L36798] ( )
UV [L36799-L36802] ( )
UB [L36803-L36886] ( $43Z , $43X = FALSE )
UN [L36887-L36889] ( )
UM [L36890-L36893] ( )
U_ [L36894-L36897] ( )
U0 [L36898-L36901] ( )
U1 [L36902-L36905] ( )
U2 [L36906-L36909] ( )
U3 [L36910-L36913] ( )
U4 [L36914-L36917] ( )
U5 [L36918-L36921] ( )
<PARSED TEXT FOR PAGE: 48 / 88>
U6 [L36922-L36925] ( )
U7 [L36926-L36929] ( )
IQ [L36930-L36933] ( )
IW [L36934-L36937] ( )
IE [L36938-L36941] ( )
IR [L36942-L36945] ( )
IT [L36946-L36949] ( )
IY [L36950-L36953] ( )
IU [L36954-L36957] ( )
II [L36958-L36961] ( )
IO [L36962-L36965] ( )
IP [L36966-L36973] ( )
IA [L36974-L36977] ( )
IS [L36978-L36981] ( )
ID [L36982-L36985] ( $15D )
IG [L36986-L36988] ( )
malvera
IH [L36989-L36992] ( $16Z )
IJ [L36993-L36996] ( )
0415
IK [L36997-L37000] ( )
IL [L37001-L37294] ( )
0F57C0 | 0F114527 | 0F114537 | 0F1145E7 | 0F57C0 | 0F1145E0 | 0F1145F0 | 0F1145A0
IZ [L37295-L37438] ( $43N , $16S = "" , $16V = FALSE )
00000000 | 00000000 | 01000000 | 00000000 | FFFFFFFF | 00000000 | 0C000000 | 00000000
IX [L37439-L37462] ( $43N , $12W )
00000000 | 01000000 | FFFFFFFF | 0C000000 | 00000000 | 00000000 | 00000000 | 00
IC [L37463-L37530] ( )
64890D | 00000000 | B9 | BF | 00000000 | 8B71 | 83FE00 | 741D
<PARSED TEXT FOR PAGE: 49 / 88>
IV [L37531-L37691] ( )
4885C9 | 7442 | 4C39F1 | 743D | 53 | 4883EC28 | 488BD9 | 488B0B
IB [L37692-L37760] ( $434 = NULL , $435 = TRUE )
byte[ | ] | byte[760] | 0x | int | ReadProcessMemory | handle | ptr
IM [L37761-L37878] ( $434 = NULL )
dword | dword | dword | dword | byte | dword | byte | byte
I_ [L37879-L37888] ( $44O , $44K , $435 = FALSE )
I0 [L37889-L37894] ( $44O , $44K , $435 = FALSE )
I1 [L37895-L37904] ( $44C )
I2 [L37905-L37914] ( $44C )
I3 [L37915-L37924] ( $44C )
I4 [L37925-L37931] ( $44K )
I5 [L37932-L37939] ( $44B )
I6 [L37940-L38038] ( $44K , $440 = FALSE )
01000000 | 01 | 48B8 | 48C700 | 01000000 | 48B8 | 48B9 | 488908
I7 [L38039-L38253] ( $44K , $132 = $21T )
48B8 | C700 | 00000000 | 00 | 48B8 | 48B9 | 8B50 | 83C2
OQ [L38254-L38408] ( $44K )
48B8 | C700 | 00000000 | 00 | 48B8 | 48B9 | 8B50 | 83C2
OW [L38409-L39009] ( $44K , $12R = $STR8218 , $443 = FALSE )
follow | follow | stand near | 00090000 | 2sqm | 00100000 | 3sqm | 00180000
OE [L39010-L39014] ( )
OT [L39015-L39032] ( )
double
OY [L39033-L39044] ( )
double
OU [L39045-L39060] ( )
double
OI [L39061-L39075] ( )
<PARSED TEXT FOR PAGE: 50 / 88>
double | double
OO [L39076-L39093] ( )
double
OP [L39094-L39099] ( )
double
OA [L39100-L39105] ( )
double
OS [L39106-L39113] ( )
double | double
OD [L39114-L39129] ( )
double
OF [L39130-L39144] ( )
double | double
OG [L39145-L39154] ( )
double
OH [L39155-L39177] ( )
dword | dword | byte | byte | byte
OJ [L39178-L39190] ( $46Y = FALSE )
dword | dword | byte
OK [L39191-L39204] ( )
dword
OL [L39205-L39218] ( )
byte | byte
OZ [L39219-L39230] ( )
OX [L39231-L39249] ( )
byte | byte
OC [L39250-L39261] ( )
byte
OV [L39262-L39272] ( )
<PARSED TEXT FOR PAGE: 51 / 88>
dword | dword
OB [L39273-L39284] ( $46D )
dword
ON [L39285-L39304] ( )
dword | dword
OM [L39305-L39336] ( $46H )
short | short | byte | byte
O_ [L39337-L39339] ( )
dword
O0 [L39340-L39358] ( )
double | byte | byte
O1 [L39359-L39367] ( $44O , $0P , $14T , $46Z , $435 = FALSE )
O2 [L39368-L39377] ( $44O , $0P , $14T , $46Z , $435 = FALSE )
O3 [L39378-L39399] ( $46X )
adeta sio | adori | adevo grav pox | adana pox | adura gran | adevo grav flam | adori flam | adito grav
O4 [L39400-L39422] ( $44O , $43Z )
O5 [L39423-L39443] ( $44O , $12B , $12O = 1 )
O6 [L39444-L39477] ( $43H , $46B = RANDOM ( 0 , 4 , 1 )
0x
O7 [L39478-L39487] ( $43H )
PQ [L39488-L39490] ( )
byte
PW [L39491-L39493] ( )
byte
PE [L39494-L39496] ( )
byte
PR [L39497-L39505] ( )
PT [L39506-L39516] ( )
double
<PARSED TEXT FOR PAGE: 52 / 88>
PY [L39517-L39525] ( )
double
PU [L39526-L39542] ( )
double
PI [L39543-L39552] ( )
double
PO [L39553-L39565] ( )
PP [L39566-L39577] ( $15D )
PA [L39578-L39958] ( $12R = 0 )
malvera
PS [L39959-L40001] ( $15Y , $48S = $18O , $48D = 0 )
Ascii | ASCII | 00 | 01 | 6A
PD [L40002-L40054] ( $48G , $15Y , $132 = RANDOM ( 200 , 350 , 1 )
Ascii | Ascii | ASCII | 00000000 | ASCII | 00000000 | 04000000 | 01
PF [L40055-L40101] ( $15Y , $48D )
Ascii | ASCII | 00000000 | 01 | 6A
PG [L40102-L40112] ( $46X , $453 = 0 , $132 = RANDOM ( 200 , 350 )
0 | exura vita ra | exura gran ra
PH [L40113-L40119] ( $48K , $0P , $14T , $46Z , $132 = RANDOM ( 550 , 800 )
0 | " | , | , | "
PJ [L40120-L40126] ( $48K , $15D , $132 = RANDOM ( 550 , 800 )
0 | "cid: | "
PK [L40127-L40164] ( $48L , $132 = RANDOM ( 550 , 800 )
PL [L40165-L40177] ( $48L = 1 )
NOT IMPLEMENTED | NOT IMPLEMENTED | FFFF0000
PZ [L40178-L40208] ( $44K , $48L , $132 = RANDOM ( 550 , 800 )
PX [L40209-L40256] ( $48L , $0P , $14T , $46Z , $132 = + 4294967295 )
PC [L40257-L40268] ( $48L , $48B = FALSE , $132 = RANDOM ( 550 , 800 )
PV [L40269-L40277] ( $48N , $48L , $48M = 1 , $132 = RANDOM ( 200 , 350 )
<PARSED TEXT FOR PAGE: 53 / 88>
PB [L40278-L40292] ( $48N , $48_ = NULL , $480 = NULL , $132 = RANDOM ( 200 , 350 )
PN [L40293-L40321] ( $483 )
PM [L40322-L40375] ( $43Z )
byte | 68 | B9 | B8 | FFD0 | 6F | 70 | 71
P_ [L40376-L40498] ( )
FAD7 | 66B8FAD7 | <not implemented> | C705 | FAD70000 | 66B9FAD7 | 48B8 | C600FA
P0 [L40499-L40602] ( )
FAD7 | D7FA | C681 | 60 | C680 | 60 | 48B9 | C601D7
P1 [L40603-L40631] ( )
B9 | B8 | FFD0 | B9 | B8 | FFD0 | 14000000 | 01
P2 [L40632-L40696] ( $43Z , $132 = 0 )
01 | 01 | 00 | 65 | 66 | 67 | 68 | 6A
P3 [L40697-L40705] ( $0P , $14T , $46Z , $48L , $48M , $132 = RANDOM ( 200 , 300 , 1 )
P4 [L40706-L40710] ( $0P , $14T , $46Z , $48L , $48M , $49I , $481 , $132 = RANDOM ( 200 , 350 , 1 )
P5 [L40711-L40722] ( $12R = 0 )
BE000000 | 01 | 6A
P6 [L40723-L40788] ( $12R = $18E )
4883EC28 | 48BA | 48B9 | 48B8 | FFD0 | 4883C428 | 4883EC28 | 48BA
P7 [L40789-L40821] ( $0P , $14T , $46Z , $49O = $19X , $46F = 2 )
00 | 4883EC38 | C7442428 | C7442420 | 48A1 | 488D40 | 49B9 | 4C8BC0
AQ [L40822-L41473] ( $0P , $14T , $46Z , $49S = FALSE , $12O = 1 , $49D = 1 , $49F = FALSE )
06 | 08 | 02 | 04 | 07 | 01 | 03 | 05
AW [L41474-L41502] ( )
byte
AE [L41503-L41549] ( )
0E | north | 11 | east | 0F | south | 10 | west
AR [L41550-L41564] ( )
03
AT [L41565-L41590] ( )
<PARSED TEXT FOR PAGE: 54 / 88>
03 | 01 | 01 | 02 | 01
AY [L41591-L41614] ( $0P , $14T , $496 = 1 )
03 | 01 | 02
AU [L41615-L41637] ( $0P , $497 , $50Q , $15D , $50W = $STR9488 )
cont | map | cont | eq | FFFF
AI [L41638-L41648] ( )
{"action":"open","data":{"npc":" | "}} | 32 | CF | Ascii
AO [L41649-L41661] ( )
{"action":"close","data":{"npc":" | "}} | 32 | CF | Ascii
AP [L41662-L41702] ( $15D , $12L , $434 = 0 )
32 | 050E | 00 | 00 | 8001 | 000001 | 00 | 00
AA [L41703-L41739] ( $15D , $12L )
32 | 050E | 00 | 00 | 8001 | 000000 | 000000 | {"action":"sell","data":{"npc":"
AS [L41740-L41765] ( )
55 | 4157 | 4156 | 4155 | 4154 | 56 | 57 | 53
AD [L41766-L41771] ( )
ushort
AF [L41772-L41787] ( )
AG [L41788-L41796] ( $37W , $463 )
, | Ascii
AH [L41797-L41805] ( $37W , $12R = $STR9564 )
gold | , | Ascii
AJ [L41806-L41814] ( $37W )
, | resetCountLimit | Ascii
AK [L41815-L41891] ( $434 = NULL )
dword | int | ReadProcessMemory | handle | ptr | ptr | int | int
AL [L41892-L41923] ( $434 = NULL )
char[36]
AZ [L41924-L41983] ( )
<PARSED TEXT FOR PAGE: 55 / 88>
48B9 | 4831D2 | 48BB | 488B33 | 488B4608 | 80781900 | 0F85 | 39000000
AX [L41984-L42067] ( $434 = NULL )
byte | dword | byte | byte | byte | byte | byte | dword
AC [L42068-L42192] ( $434 = NULL , $50F = NULL )
byte | dword | byte | byte[8] | int | ReadProcessMemory | handle | ptr
AV [L42193-L42195] ( )
AB [L42196-L42215] ( $48_ = NULL )
AN [L42216-L42230] ( )
AM [L42231-L42281] ( $48L , $46H = NULL , $51O = FALSE , $51P = NULL )
A_ [L42282-L42293] ( )
A0 [L42294-L42335] ( $46H = NULL )
byte[ | ] | 0x | int | ReadProcessMemory | handle | ptr | ptr
A1 [L42336-L42535] ( )
4831DB | 4831F6 | 4883FB | 0F8F | D5000000 | 49B8 | 498B04D8 | 4883F800
A2 [L42536-L42587] ( $434 = NULL , $50F = NULL )
byte[ | ] | 0x | 0x | byte[ | ] | 0x | 0x
A3 [L42588-L42657] ( $48L , $44C , $48Z = 0 )
FF | FFFF | FFFF | FF | FF | FFFF | 0000 | 00
A4 [L42658-L42726] ( $48L , $0P , $14T , $46Z )
FFFF | FFFF | FF | FF | FFFF | 0000 | 00 | FF
A5 [L42727-L42744] ( $48L , $48B = FALSE )
FFFF | 0000 | 00 | 00 | not implemented
A6 [L42745-L42956] ( $51K , $481 , $48L , $0P , $14T , $46Z , $44C = NULL , $43K = NULL )
FFFF0000 | 68 | 68 | 68 | 68 | 68 | 68 | B9
A7 [L42957-L43028] ( $51_ , $510 , $511 , $512 , $513 , $514 )
FFFF0000 | FFFF0000 | 68 | 68 | 68 | 68 | 68 | 68
SQ [L43029-L43090] ( $51K , $481 , $48L , $44K )
FFFF0000 | 68 | 68 | 68 | 68 | B9 | B8 | FFD0
SW [L43091-L43153] ( $51_ , $510 , $512 , $513 , $48L , $50B = 1 , $132 = 0 )
<PARSED TEXT FOR PAGE: 56 / 88>
FFFF | FFFF | 00 | 05000000 | FFFFFFFF | FFFF0000 | 0000 | 0000
SE [L43154-L43212] ( $51_ , $510 , $0P , $14T , $46Z , $48L , $50B , $132 = 0 )
FFFF | 00 | FFFF0000 | 68 | 68 | 68 | 68 | B9
SR [L43213-L43289] ( $51K , $481 , $48L , $52V = FALSE , $132 = 0 )
FFFF0000 | 68 | 68 | 68 | 68 | B9 | B8 | FFD0
ST [L43290-L43362] ( $0P , $14T , $46Z , $48L , $50B = 1 , $512 = 64 , $513 = 0 )
FFFF | 00 | FFFF0000 | B9 | 68 | 68 | 68 | 01000000
SY [L43363-L43377] ( $51K )
87000000 | 01 | 6A
SU [L43378-L43392] ( $51K )
88000000 | 01 | 6A
SI [L43393-L43700] ( $52B = FALSE )
dword | dword | dword | dword | dword | dword | dword | dword
SO [L43701-L43724] ( )
SP [L43725-L43732] ( )
SA [L43733-L43740] ( $524 )
dword | dword
SS [L43741-L43750] ( $524 )
SD [L43751-L43771] ( )
SF [L43772-L43794] ( $44W , $44E )
SG [L43795-L43797] ( )
SH [L43798-L43805] ( )
SJ [L43806-L43811] ( )
SK [L43812-L43819] ( )
char[36]
SL [L43820-L43830] ( )
SZ [L43831-L43846] ( )
char[36] | char[36]
SX [L43847-L43854] ( )
<PARSED TEXT FOR PAGE: 57 / 88>
char[36] | char[36]
SC [L43855-L43860] ( $11Y , $53I , $53O )
char[16] | char[16]
SV [L43861-L43865] ( $11Y , $121 )
SB [L43866-L43915] ( )
BF | 8B5D08 | B9 | FFE1 | 8D4902 | 8039 | 83 | 7550
SN [L43916-L43931] ( $53A = FALSE )
char[ | ]
SM [L43932-L44173] ( $53A = FALSE )
char[36] | char[36] | 01 | : | char[36] | 01 | You are not the owner. | 01
S_ [L44174-L44223] ( )
 [ | ] | You are not the owner. | There is not enough room. | Sorry, not possible. | You are not the owner. | This 
object is too heavy for you to carry. | You do not have enough capacity.
S0 [L44224-L44424] ( )
4833C4 | 48898424 | 88000000 | 418BF1 | 50 | 48B8 | 448900 | 58
S1 [L44425-L44458] ( )
4831C09090 | EB6C909090 | E950020000 | EB31 | EB23 | EB36 | E9 | EA000000
S2 [L44459-L44556] ( )
BF00000000 | 8A01 | 8802 | 8887 | 83C701 | 41 | 42 | 84C0
S3 [L44557-L44582] ( )
A3 | 8915 | 8B4D08 | 8B09 | 8B4924 | B800000000 | 88C8 | 8888
S4 [L44583-L44663] ( )
6578706C6F7265722E657865 | 617564696F64672E657865 | 63737273732E657865 | 737663686F73742E657865 | 
576D6950727653452E657865 | 6C736173732E657865 | 7365727669636573732E657865 | 4C4D532E657865
S5 [L44664-L44700] ( )
{"processesRunning":
{"TfmSecond":false,"TinyTaskClass":false,"AutoHotkeyGUI":false,"KasteriaOTBot":false,"MiniMouseMacro":false,"Auto
Itv3GUI":false,"FMTmrMainFMX:false"}} | 32 | Ascii | A3 | 8915 | 8B4D08 | 8B09 | 8B4928
S6 [L44701-L44729] ( )
A3 | 8915 | 8B4D08 | 8B09 | 8B4924 | 80F9FF | 7509 | 8B4D08
<PARSED TEXT FOR PAGE: 58 / 88>
S7 [L44730-L44753] ( )
890D | 8915 | 8B4D08 | 8B09 | 8B491A | B800000000 | 88C8 | 8888
DQ [L44754-L44792] ( )
handle | wstr | kernel32.dll | ptr | handle | str | kernel32.dll | 50
DW [L44793-L44817] ( )
8B4508 | 8B00 | 8B5824 | B800000000 | 88D8 | 3C9F | 7418 | 3C9F
DE [L44818-L44845] ( )
51 | 8B4308 | 8B00 | 8B4824 | B800000000 | 88C8 | 3C1F | 7419
DR [L44846-L44872] ( )
A1 | 8903 | A1 | 894304 | A1 | 894308 | A1 | 89430C
DT [L44873-L44904] ( )
8B4508 | 8B00 | 8B401C | 8B580C | 80FB32 | 7526 | 81FB32982500 | 742E
DY [L44905-L44931] ( )
8B4508 | 8B00 | 8B5824 | B800000000 | 88D8 | 8880 | 3C9F | 7418
DU [L44932-L44940] ( )
/modules/game_commerce/commerce_banner.otui | 9F000000 | 000000 | Ascii
DI [L44941-L44979] ( )
8B4508 | 8B00 | 8B5824 | B800000000 | 88D8 | 3CC8 | 7418 | 3CC8
DP [L44980-L45018] ( )
8B4508 | 8B00 | 8B5824 | B800000000 | 88D8 | 3C00 | 743B | 3C02
DA [L45019-L45062] ( )
8B4508 | 8B00 | 8B5824 | 80FB40 | 0F85 | 15000000 | C74024 | 1E000000
DS [L45063-L45159] ( )
handle | GetModuleHandleW | wstr | ntdll.dll | ntdll.dll | handle | GetModuleHandleW | wstr
DD [L45160-L45173] ( )
handle | GetModuleHandleW | wstr | kernelbase.dll | kernelbase.dll | ptr | GetProcAddress | handle
DF [L45174-L45290] ( )
C21000 | C21400 | C21800 | handle | GetModuleHandleW | wstr | ntdll.dll | ntdll.dll
DG [L45291-L45293] ( )
<PARSED TEXT FOR PAGE: 59 / 88>
E996020000
DH [L45294-L45318] ( )
8B4508 | 8B00 | 8B5824 | B800000000 | 88D8 | 3C53 | 7418 | 3C50
DJ [L45319-L45343] ( )
8B4508 | 8B00 | 8B5828 | B800000000 | 88D8 | 3C55 | 7418 | 3C53
DK [L45344-L45363] ( $46H , $542 = FALSE )
0 | i | 1 | h | 2 | k | 3 | j
DL [L45364-L45377] ( )
[REGEXPCLASS:(?i)( | tfmappclass | )] | 8B | 0D | 00000000 | C3 | int
DZ [L45378-L45456] ( )
48895C2408 | 55 | 56 | 57 | 4156 | 4157 | 488D6C24C9 | 4055
DX [L45457-L45561] ( )
: | byte | Ascii | Ascii | 48B8 | Ascii | 488901 | 48B8
DC [L45562-L45573] ( )
[REGEXPCLASS:(?i)( | )]
DV [L45574-L45576] ( )
E961020000
DB [L45577-L45598] ( )
C745E4 | 00000000 | 8B0D | 39F9 | 7527 | 8B49 | 890D | 8B0D
DN [L45599-L45636] ( )
A3 | 8915 | 8B4D08 | 8B09 | 8B4928 | 31C0 | 88C8 | 8888
DM [L45637-L45644] ( )
byte | 62
D_ [L45645-L45657] ( )
C640FF | 8B8558FFFFFF | 83C40C | B9 | FFE1 | B9 | FFE1
D0 [L45658-L45674] ( )
[REGEXPTITLE:(?i)( | pureplay | )] | ntdll.dll | int | NtSuspendProcess | handle
D1 [L45675-L45682] ( )
ProjectAntica.Guard.exe | kernel32.dll | handle | OpenProcess | dword | bool | dword | ntdll.dll
<PARSED TEXT FOR PAGE: 60 / 88>
D2 [L45683-L45693] ( $55D = FALSE )
Aegis.exe | kernel32.dll | handle | OpenProcess | dword | bool | dword | ntdll.dll
D3 [L45694-L45701] ( )
OTRivaliaLauncher.exe | kernel32.dll | handle | OpenProcess | dword | bool | dword | ntdll.dll
D4 [L45702-L45705] ( )
E9A400000090
D5 [L45706-L46345] ( )
A1 | B9 | 0FAFC8 | B8 | 01C1 | 8B45EC | 8901 | 8B45E8
D6 [L46346-L46526] ( )
9C | 50 | 53 | 51 | 52 | 56 | 57 | 55
D7 [L46527-L46565] ( BYREF $12M )
byte[ | ] | char[ | ] | char[ | ] | exiva | "
FQ [L46566-L46568] ( )
FW [L46569-L46913] ( )
B8 | 8908 | 895004 | 897008 | 89580C | A1 | B9 | 0FAFC8
FE [L46914-L47067] ( )
9C | 50 | 53 | 51 | 52 | 56 | 57 | 55
FR [L47068-L47091] ( BYREF $12M )
dword | dword | char[ | ]
FT [L47092-L47094] ( )
FY [L47095-L47158] ( )
B8 | 8908 | 895004 | 897008 | 89580C | A1 | B9 | 0FAFC8
FU [L47159-L47187] ( BYREF $12M )
byte[8] | 0x | 0x | 0x | 0x | char[ | ]
FI [L47188-L47190] ( )
FO [L47191-L47208] ( $15Y )
FP [L47209-L47220] ( $46X )
FA [L47221-L47686] ( )
48BF | 48B9 | FFFFFF | FFFFFFFF | 48890F | 48B9 | FFFFFF | FFFFFFFF
<PARSED TEXT FOR PAGE: 61 / 88>
FS [L47687-L47784] ( $0P , $14T , $46Z , $44O = NULL , $55L = FALSE )
bool | ReadProcessMemory | handle | ptr | ptr | int | int | x
FD [L47785-L47820] ( $0P , $14T , $46Z , $44O = NULL )
bool | ReadProcessMemory | handle | ptr | ptr | int | int | x
FF [L47821-L47837] ( $0P , $14T , $46Z , $44O = NULL )
bool | ReadProcessMemory | handle | ptr | ptr | int | int | x
FG [L47838-L47851] ( $0P , $14T , $46Z , $15D , $44O = NULL )
FH [L47852-L47888] ( $15D , $56R )
int | int | uint64 | uint64 | ReadProcessMemory | handle | ptr | ptr
FJ [L47889-L47917] ( $15D , $56R , $46F = 0 )
FK [L47918-L47933] ( )
56 | 4883EC20 | 4889CE | 4889F0 | 4883C420 | 5E | C3 | C20400
FL [L47934-L47950] ( $0P , $14T , $46Z , $44O = NULL , $56O = 2 , $56P = 2 )
FZ [L47951-L48063] ( )
49B8 | 41833801 | 0F85 | CA000000 | 49B8 | 498B08 | 498B4008 | 4829C8
FX [L48064-L48084] ( )
byte[ | ]
FC [L48085-L48251] ( )
81FE | FFFFFFFF | 7473 | 83FE03 | 7E6E | 8B711C | A1 | 8B400C
FV [L48252-L48346] ( $49D = 7 )
Not Implemented | Outdated func | byte | short | byte
FB [L48347-L48356] ( $53W , $15D )
FN [L48357-L48445] ( $0P , $14T , $46Z , $48L = 0 , $132 = + 4294967295 , $51G = 1 , $56M = FALSE )
68 | 68 | 68 | 68 | B9 | B8 | FFD0 | 68
FM [L48446-L48506] ( $56_ , $560 , $561 , $562 , $563 , $564 , $48L , $50B , $132 = RANDOM ( 550 , 800 , 1 )
00 | 78 | 6A | 01 | 6A
F_ [L48507-L48530] ( $0P , $14T , $46Z , $132 = 300 )
: | : | : | : | Ascii | 00
F0 [L48531-L48554] ( $56_ , $560 , $561 , $562 , $563 , $564 , $132 = RANDOM ( 550 , 800 , 1 )
<PARSED TEXT FOR PAGE: 62 / 88>
<not implemented>
F1 [L48555-L48606] ( )
dword | int | ReadProcessMemory | handle | ptr | ptr | int | int
F2 [L48607-L48621] ( $37W , $57Y , $57U , $57I = 0 )
F3 [L48622-L48631] ( $37W , $57Y )
dword | dword
F4 [L48632-L48642] ( $37W , $57Y )
byte
F5 [L48643-L48685] ( $37W )
dword | byte | char[36]
F6 [L48686-L48698] ( $37W )
F7 [L48699-L48713] ( $37W , $12U , $57J )
byte
GQ [L48714-L48723] ( $13I , $132 = 0 )
Log | : | : | | :
GW [L49598-L49611] ( $43N )
GE [L49612-L49622] ( )
GR [L49623-L49724] ( )
/ | / | / | / | / | / | / | /
GT [L49725-L49738] ( $12T )
GY [L49739-L49747] ( )
GU [L49748-L49756] ( )
GI [L49757-L49768] ( $81S )
+++ Error: | . Nie udana proba wyslania zapytania UDP.
GO [L49769-L49786] ( )
0415 | B��d po��czenia | Serwer nie odpowiada na twoje po��czenie. Wyst�pi� problem z hostem, musisz 
uzbroi� si� w cierpliwo��. Odwied�: rifbot.eu i sprawd� czy s� nowe informacje. | Connection error | Server 
not responding for your connection. There is problem with host, you need to be patient. Visit: rifbot.eu and check if 
any news. | Error
GP [L49787-L49789] ( )
<PARSED TEXT FOR PAGE: 63 / 88>
GA [L49790-L49795] ( $12T = "" )
GS [L49796-L49802] ( $12T = $39E & $STR14040 & G0 ( )
/ | /exit
GD [L49803-L49808] ( $12T = "" )
GF [L49809-L49814] ( $12T = "" )
GG [L49815-L49823] ( )
MS Sans Serif | shell32.dll | Your license has expired. What to do? | Reset Key | Purchase
GH [L49824-L53301] ( $43N )
/ | / | / | : | Rifbot | 0x | 0x | 0x
GJ [L53302-L53305] ( BYREF $125 , $11G )
GK [L53306-L53308] ( )
GL [L53309-L53340] ( $81Z )
\download.exe | \Rifbot.exe | Rifbot | Rifbot_x64 | Updating | please wait.. | AutoIt.Error | 
WinHttp.WinHttpRequest.5.1
GZ [L53341-L53390] ( )
<start> | <start64> | <end> | total= | <64>0 | 0x | <end> | <64>
GX [L53391-L53403] ( $12_ )
GC [L53404-L53417] ( $814 = NULL )
delme.bat | :Repeat | attrib | -r -s | del | if exist | goto Repeat | start /d
GV [L53418-L53430] ( )
.exe | \ | download.exe
GB [L53431-L53441] ( )
rifbot.exe
GN [L53442-L53701] ( $43N )
tibia.exe | Zagan.exe | Error | Nie uda�o si� wczyta� podzespo��w | Error | Failed to load resources. | 
malvera | FFFFFF
GM [L53702-L53708] ( )
G_ [L53709-L53725] ( $817 = $38A )
- | -
G0 [L53726-L53730] ( )
<PARSED TEXT FOR PAGE: 64 / 88>
& | Ascii | & | Ascii | & | Ascii | Ascii
G1 [L53731-L53741] ( )
G2 [L53742-L53765] ( )
G3 [L53766-L53811] ( )
MS Sans Serif | shell32.dll | Enter your serial number to continue. | - | You can paste entire key here | - | - | -
G4 [L53812-L53816] ( )
G5 [L53817-L53821] ( )
Rifbot
G6 [L53822-L53863] ( )
Edit1 | Edit2 | Edit3 | Edit4
G7 [L53864-L53869] ( $82A , $82S )
HQ [L53870-L53881] ( $82D = FALSE )
Error
HW [L53882-L53889] ( )
HE [L53890-L53898] ( )
HR [L53899-L53912] ( )
0415 | Nie mo�na zapisa� twojego klucza, uruchom bota jako admin nast�pnym razem. | Failed to save your 
serial number, run bot as admin next time.
HT [L53913-L54144] ( )
 SDC | RT | N | R8 | R | RN | M | C
HY [L54145-L54174] ( $81F )
days | 0 | day | day | day | h | h | left
HU [L54175-L54271] ( )
--[[ | Script Name: Auto SSA | Description: Automatically equip Stone Skin Amulet 
when slot.amulet is empty. | Author: Ascer - example | ]] | local SSA = 3081 -- id of stone 
skin amulet | Module.New("Auto SSA", function (mod) -- declare module name and set function mod | if 
Self.Amulet().id == 0 then -- get currnet amulet.id
HI [L54272-L54277] ( )
\Rifbot | \Rifbot\config.txt | \Rifbot\Scripts\ | \Rifbot\Settings\
HO [L54278-L54295] ( )
<PARSED TEXT FOR PAGE: 65 / 88>
\ | rifbot_version | " | "
HP [L54296-L54325] ( $43N )
Rifbot | / | Rifbot | 0x | 0x | 0x | dword
HA [L54326-L54328] ( )
HS [L54329-L54335] ( )
HD [L54336-L54364] ( )
HF [L54365-L54381] ( $82X = FALSE )
\check.txt | 1 | User | Continue without Admin? Some func won't work. | 0415 | Kontynuuj bez Admina? Niekt�re 
func nie b�d� dzia�a�. | Admin required
HG [L54382-L54388] ( )
.
HH [L54389-L54411] ( )
byte | 0xB8
HJ [L54412-L54418] ( )
kernel32.dll | ptr | GetCurrentThread | kernel32.dll | bool | SetThreadPriority | ptr | int
HK [L54419-L54457] ( )
0415 | Wybrany klient gry ma ju� aktywnego Rifbota. Kliknij na okno gry do kt�rej chcesz si� po��czy� a potem
uruchom bota. | Selected game client already have actived Rifbot. Click on game window that you want to connect 
and run bot. | dword | byte[ | ] | 0x | byte[
HL [L54458-L54496] ( )
Cavebot: Stopping walker, targeting, looter due character death.
HZ [L54497-L54531] ( )
HX [L54532-L54542] ( )
Tahoma | MS Sans Serif
HC [L54543-L54550] ( )
HV [L54551-L54582] ( )
HB [L54583-L54628] ( )
\Settings | *.* | :: Exiting... | :: Exiting...
HN [L54629-L54632] ( )
HM [L54633-L54640] ( $83Q = FALSE )
<PARSED TEXT FOR PAGE: 66 / 88>
H_ [L54641-L54654] ( $83Q = FALSE )
H0 [L54655-L54658] ( )
H1 [L54659-L54662] ( )
H2 [L54663-L54666] ( )
H3 [L54667-L54670] ( )
H4 [L54671-L54674] ( )
H5 [L54675-L54682] ( )
H6 [L54683-L54687] ( )
H7 [L54688-L54691] ( )
JQ [L54692-L54707] ( $83W = TRUE , $83E = FALSE )
JW [L54708-L54772] ( )
0000000000 | [ | ] Rifbot said code [ | ] on default. Random time for respond was | s. | Macro Test | [ | ] Rifbot failed
to say code [
JE [L54773-L54802] ( )
^p | ^s | ^o | ^l | ^m | ^t | ^j | ^g
JR [L54803-L54817] ( )
JT [L54818-L54821] ( )
JY [L54822-L54830] ( $83W = FALSE )
JU [L54831-L54845] ( $83U )
JI [L54846-L54854] ( $83U , $83O , $83P )
JO [L54855-L54866] ( $83U , $132 )
JP [L54867-L54878] ( $83U )
JA [L54879-L54897] ( $12R )
low | mid | high | extra high | heal rune | healing rune | attack rune
JS [L54898-L54903] ( $132 = 1000 )
JD [L54904-L54927] ( )
[REGEXPCLASS:(?i)( | )]
JF [L54928-L54933] ( )
Background mode, click to restore.
<PARSED TEXT FOR PAGE: 67 / 88>
JG [L54934-L54938] ( )
JH [L54939-L54967] ( )
JJ [L54968-L54971] ( )
JK [L54972-L55031] ( )
JL [L55032-L55035] ( )
JZ [L55036-L55065] ( )
User | 0415 | Brak uprawnie� admina, uruchom bota jako admin i spr�buj ponownie. | Admin rights required, run
bot as admin and try again. | 0415 | Windows Defender: Przetwarzam.. | Windows Defender: Working.. | powershell
-Command
JX [L55066-L55087] ( $0A , $2E , $0U , $2R )
user32.dll | bool | MoveWindow | hwnd | int | int | int | int
JC [L55088-L55101] ( $0A , $15Y , $0U , $2R )
int;int;int;int;int;int;int;int;int;int | int;int;int;int;int;int;int;int;int;int
JV [L55102-L55112] ( $13I , $57K , $51D , $83H , $83G , $83K = "" , $83L = 32 , $83Z = 32 , $83X = 5 , $83C = + 
4294967289 )
JB [L55113-L55221] ( )
Healing | healing.ico | Light: | 0 | % | 25 | Mana | exura
JN [L55227-L55273] ( )
25 | 27 | D | 20 | no items in table | not connected | no bp | adding:
JM [L55274-L55323] ( )
selfHealer | heal rune | heal rune | heal rune | heal rune | heal rune | heal rune | low
J_ [L55324-L55341] ( )
\Friends.txt | Friends list, one name per line. | Friend1 | Friend2 | friends
J0 [L55342-L55357] ( )
\Friends.txt | ;
J1 [L55358-L55403] ( )
exura sio " | exura min sio " | incuro par " | incuro min par " | exana sio " | exura sio mini " | magna curatio " | 
exura gran sio "
J2 [L55404-L55455] ( $12T )
<PARSED TEXT FOR PAGE: 68 / 88>
mp | mana | lightmana | mana potion | manapotion | mf | manafluid | mana fluid
J3 [L55456-L55586] ( )
Tools | tools.ico | AntiPara: | exura | Mana Shield | AutoHur: | utani gran hur | Lighthack
J4 [L55587-L55610] ( )
holdPos
J5 [L55611-L55697] ( )
mid | fishing
J6 [L55698-L55787] ( )
spearPickpup | mid
J7 [L55788-L55816] ( )
conditions | aneva vita | mana protectio | utamo vita
KQ [L55817-L55838] ( )
advertising | advertisingDefault | advertising
KW [L55839-L55864] ( )
Check Item ID | {id = | , x = | , y = | , z = | } *pos.z is self.z | Object id | {id = | , x =
KE [L55865-L55877] ( )
dword
KR [L55878-L56043] ( )
autoLoot | \d+ | \d+ | \d+ | \d+ | \d+ | \d+ | \d+
KT [L56044-L56060] ( )
refilAmmo
KY [L56061-L56094] ( )
PVP Area | pvp area.ico | Hold Target | McShortkeys | SD Combo: | 30 | % | Ering:
KU [L56095-L56107] ( )
low
KI [L56108-L56120] ( )
high
KO [L56121-L56142] ( )
holdTarget | 1B | mid
<PARSED TEXT FOR PAGE: 69 / 88>
KP [L56143-L56155] ( )
runeMax | high
KA [L56156-L56166] ( )
autoRing | mid
KS [L56167-L56178] ( )
autoAmulet | mid
KD [L56179-L56222] ( )
Runemaker | runemaker.ico | Enabled | 70 | % Spell: | adura vita | Player Logout | Relogin After:
KF [L56223-L56299] ( )
runemaker | No Blank Runes | [ | ] Your character has been logout due out of blank runes or soul points. | [ | ] 
Twoja posta� zosta�a wylogowana bo zabrak�o blank runek lub souli. | No Blank Runes | [
KG [L56300-L56509] ( )
0415 | bo wykryto gracza: | INTERNET OR GAME | bo utracono po��czenie lub pad�a posta�. | Reason player 
detected: | INTERNET OR GAME | Reason the internet or game issue. | Relogin
KH [L56510-L56543] ( )
KJ [L56544-L56570] ( )
low
KK [L56571-L56582] ( )
KL [L56583-L56642] ( )
select character | select character | enter game
KZ [L56643-L56674] ( )
Alarms | alarms.ico | Walker Stuck | Low Health: | 50 | % | Private Message | Out Of Blanks
KX [L56675-L56686] ( )
Ok | Welcome in private message proxy console.
KC [L56687-L56704] ( $864 = $STR15133 , $132 = 1500 )
Player Attack.mp3 | \Alarms\ | .mp3 | .mp3 | .wav
KV [L56705-L56710] ( $132 = 1500 )
KB [L56711-L56753] ( )
0415 | [ | ] | w�a�nie ci� zaatakowa�. | [ | ] | just attacked you. | Player Attack
KN [L56754-L56805] ( )
<PARSED TEXT FOR PAGE: 70 / 88>
Private Message.mp3 | Out Of Blanks.mp3 | Macro Test.wav | History ( | ) | Private Message | [ | ]
KM [L56806-L56812] ( )
Disconnected.mp3
K_ [L56813-L56835] ( $53H , $53L )
GetSelected | | : | GetLineCount | GetLineCount
K0 [L56836-L56847] ( )
Copy | Copy name | Copy message | Select all | Clear console | ptr | hwnd;uint;wparam;lparam
K1 [L56848-L56860] ( )
K2 [L56861-L56870] ( )
K3 [L56871-L56889] ( $12R = 0 )
GetSelected
K4 [L56890-L56898] ( )
Clear console | Do you want to delete all messages? | Console was cleared successfully. | History
K5 [L56899-L56929] ( $0A , $15Y , $0U , $2R )
user32.dll | int | CallWindowProc | ptr | hwnd | uint | wparam | lparam
K6 [L56930-L56941] ( )
No Mana Regen.mp3
K7 [L56942-L56956] ( )
, | VIP Online.mp3
LQ [L56957-L56962] ( )
Walker Stuck.wav
LW [L56963-L56971] ( )
Low Health.wav
LE [L56972-L57055] ( )
Shortkeys | shortkeys.ico | F1 | meuse 3160 | F2 | exivat | F3 | ering
LR [L57056-L57063] ( )
LT [L57064-L57291] ( $43H )
0 | <empty> | execute | say | low | exivat | exiva " | low
LY [L57292-L57352] ( )
<PARSED TEXT FOR PAGE: 71 / 88>
short | GetAsyncKeyState | int | 0x11 | short | GetAsyncKeyState | int | 0x50
LU [L57353-L57378] ( )
\Shortkeys.txt | additional | ; | ; | 0x
LI [L57379-L57436] ( )
\Shortkeys.txt | Shortkeys list execute once, default delay=100ms. Read below to get more information about each 
function. | Important! Make sure that item id is valid. | say exura - Say text on Default channel. | exivat
- Exiva current target or last attacked player. | exival - Exiva your last 
exived player. | ring 2167 - Equip itemid to ring slot. | amulet 2316 - Equip itemid to amulet 
slot.
LO [L57437-L57461] ( $83W = TRUE )
LP [L57462-L57487] ( )
Information Box | info.ico | Function | v | License | Information Box | Function | v
LA [L57488-L57503] ( $88T , $16S , $15Y , $81K = "" , $88Y = TRUE , $88U = $59D )
0415 | : | : | | :
LS [L57504-L57515] ( )
LD [L57516-L57677] ( )
Light Healing | Enter health percent your character. Priority=3 | Wpisz procentow� warto�� �ycia swojej postaci.
Priorytet=3 | Light Healing | Enter mana points need to cast spell. | Wpisz ilo�� punkt�w many potrzebn� do 
wypowiadania czaru. | Light Healing | Enter spell, id or shortcut e.g uhp, shp, gsp, suhp to heal your character.
LF [L57678-L57687] ( )
Copy | Show All | ptr | hwnd;uint;wparam;lparam
LG [L57688-L57696] ( )
user32.dll | int | TrackPopupMenuEx | hwnd | int | int | int | hwnd
LH [L57697-L57719] ( $0A , $15Y , $0U , $2R )
user32.dll | int | TrackPopupMenuEx | hwnd | int | int | int | hwnd
LJ [L57720-L57726] ( )
GetSelected
LK [L57727-L57736] ( $88S , $88D )
dword Size;INT Mask;dword Style;uint YMax;handle hBack;dword ContextHelpID;ulong_ptr MenuData | Mask | Style
| Size | user32.dll | bool | SetMenuInfo | handle
LL [L57737-L57740] ( )
LZ [L57741-L57747] ( )
<PARSED TEXT FOR PAGE: 72 / 88>
Key | ptr | hwnd;uint;wparam;lparam
LX [L57748-L57768] ( $0A , $15Y , $0U , $2R )
user32.dll | int | TrackPopupMenuEx | hwnd | int | int | int | hwnd
LC [L57769-L57773] ( )
LV [L57774-L57813] ( )
&File | Open.. | {Ctrl+O} | Save as.. | {Ctrl+S} | Exit program | Options | Pause Bot
LB [L57814-L57881] ( )
ENG | 0415 | PL | &Language | English - ENG | Polish - PL | &Help | More Info
LN [L57882-L57894] ( $37W , $50U )
LM [L57895-L57914] ( $82A )
 ENG | PL
L_ [L57915-L57918] ( $88P = $STR15976 )
ENG | Language:
L0 [L57919-L57922] ( )
Cavebot | cavebot.ico
L1 [L57923-L58027] ( )
Walker | Record Nodes | node | weak node | stand | center | ^ north | > east
L2 [L58028-L58109] ( )
, | Counter ID: | , | Depot ID: | , | Ladder ID: | , | Rope ID:
L3 [L58110-L58124] ( )
Pathable ID | Cancel | Apply | {Enter}
L4 [L58125-L58137] ( )
Enter time in ms. 1000ms = 1s | Ok | Cancel | {Enter}
L5 [L58138-L58150] ( )
Enter key id or leave blank. | Ok | Cancel | {Enter}
L6 [L58151-L58163] ( )
Enter name of label | Ok | Cancel | {Enter}
L7 [L58164-L58185] ( )
Condition: | none | Param: | > | Value: | If condition = true go to label: | Ok | Cancel
<PARSED TEXT FOR PAGE: 73 / 88>
ZQ [L58186-L58199] ( )
Enter message to say | npc | Ok | Cancel | {Enter}
ZW [L58200-L58215] ( )
Where to mine? | nw | n | ne | w | e | sw | s
ZE [L58216-L58231] ( )
Where to cut? | nw | n | ne | w | e | sw | s
ZR [L58232-L58247] ( )
Where to step? | nw | n | ne | w | e | sw | s
ZT [L58248-L58265] ( )
Lucida Console | Ok | Cancel | Line: | {Enter}
ZY [L58266-L58272] ( )
Line: | GetCurrentLine | Line:
ZU [L58273-L58285] ( )
Enter bps id to open from main backpack. | Ok | Cancel | {Enter}
ZI [L58286-L58300] ( )
To backpack id inside depot (if 0 will deposit to locker) | 0 | Enter items id to deposit: | 3031, 3035 | Ok | Cancel | 
{Enter}
ZO [L58301-L58319] ( )
From bp id inside depot (if 0 will withdraw from locker): | 0 | To container index (0 = first open bp, 1 = second bp): |
0 | Amount (if 0 will withdraw to max cap/space): | 1 | Enter items id to withdraw: | 2148, 2152
ZP [L58320-L58344] ( )
Buy | Sell | Buy items id up to count: {id, count}, {id2, count2} | {3160, 100}, | Ok | Cancel | Enter items like this: 
{name, id}, {name2, id2} | {sabre, 2385},
ZA [L58345-L58357] ( )
Sell all visible items: id1, id2, id3 | {3160, 100}, | 2385, 1450, | Buy items id up to count: {id, count}, {id2, count2} | 
2385, 1450, | {3160, 100},
ZS [L58358-L58370] ( )
Enter one line script. Don't use infinite loops and modules. | Ok | Cancel | {Enter}
ZD [L58371-L58383] ( )
Enter items id to store inside ground container. | Ok | Cancel | {Enter}
<PARSED TEXT FOR PAGE: 74 / 88>
ZF [L58384-L58400] ( )
Module: | targeting|walker|looter|cavebot|lure | targeting | State: | on|off | off | Ok | Cancel
ZG [L58401-L58412] ( )
targeting | on|off|if stuck | if stuck | walker | on|off | on | on|off | on
ZH [L58413-L58455] ( )
none | level | cap | exp | soul | hpperc | mpperc | item
ZJ [L58456-L58682] ( $12R = $STR16218 , $88_ = FALSE )
node | Walker: Login to game to add waypoint. | node | node: | , | , | weak node | weak node:
ZK [L58683-L58691] ( )
ZL [L58692-L58699] ( )
Clear Waypoints | Will delete all waypoitns continue?
ZZ [L58700-L58712] ( $81C = TRUE )
ZX [L58713-L58726] ( $81C = TRUE , $52Q = NULL )
ZC [L58727-L58738] ( $81C = TRUE )
ZV [L58739-L58750] ( $81C = TRUE )
ZB [L58751-L58762] ( $81C = TRUE )
ZN [L58763-L58774] ( $81C = TRUE )
ZM [L58775-L58787] ( $81C = TRUE , $53L = NULL )
Z_ [L58788-L58798] ( $81C = TRUE )
Z0 [L58799-L58809] ( $81C = TRUE )
Z1 [L58810-L58820] ( $81C = TRUE )
Z2 [L58821-L58834] ( $81C = TRUE , $37W = NULL )
Z3 [L58835-L58846] ( $81C = TRUE )
Z4 [L58847-L58858] ( $81C = TRUE )
Z5 [L58859-L58870] ( $81C = TRUE )
Z6 [L58871-L58882] ( $81C = TRUE )
Z7 [L58883-L58904] ( $81C = TRUE , $882 = NULL , $880 = NULL , $11G = NULL , $88A = NULL )
XQ [L58905-L58950] ( $81C = TRUE , $883 = FALSE )
XW [L58951-L58957] ( $13I )
<PARSED TEXT FOR PAGE: 75 / 88>
XE [L58958-L59005] ( $434 = NULL , $887 = NULL )
Walker (goto) failed to find label:
XR [L59006-L59060] ( $89Q = TRUE )
XT [L59061-L59072] ( )
XY [L59073-L59081] ( $0P , $14T , $46Z )
XU [L59082-L59093] ( )
XI [L59094-L59102] ( )
byte | byte
XO [L59103-L59155] ( $44O )
ladder | rope | sewer | shovel | ladder | rope
XP [L59156-L59173] ( $44O )
XA [L59174-L59265] ( )
Record Nodes | ladder | rope | : | , | , | sewer | shovel
XS [L59266-L59273] ( )
XD [L59274-L59376] ( $89L = TRUE )
, | , | , | , | , | , | , | ,
XF [L59377-L59468] ( $89L = TRUE , $89Z = + 4294967295 , $89X = + 4294967295 )
rope id | shovel id | pick id | mine pick id | machete id | pathable id | counter id | depot id
XG [L59469-L59475] ( $12C )
,
XH [L59476-L59489] ( BYREF $12C , $12T , $11A = FALSE )
,
XJ [L59490-L59496] ( $12C )
,
XK [L59497-L59575] ( )
Rope ID | Shovel ID | Pick ID | Mine Pick ID | Machete ID | Pathable ID | Counter ID | Depot ID
XL [L59576-L59583] ( $49Z = 1 , $132 = $22L )
XZ [L59584-L59611] ( $83W = TRUE )
Record Nodes
<PARSED TEXT FOR PAGE: 76 / 88>
XX [L59612-L59685] ( $0P , $14T , $46Z , $44O , $89B = FALSE )
XC [L59686-L59689] ( )
XV [L59690-L59695] ( )
luring
XB [L59696-L59704] ( $83W = TRUE )
Walker luring | Walker
XN [L59705-L59715] ( $83W = TRUE )
XM [L59716-L59719] ( )
X_ [L59720-L59738] ( )
dword
X0 [L59739-L62394] ( $89M = FALSE )
locker | Walker: Ignore wpt due max tries | Walker: Ignore wpt due max use tries | node: | weak node: | node: | , | 
stand:
X1 [L62395-L62434] ( )
Targeting | Mode: | follow | Attack Any Monster | Should Attack Me | slime,rotworm | HP: | 0
X2 [L62435-L62472] ( )
Priority from the top | magic shooter.ico | target spell | Spell Name/Rune ID: | exori con | Mobs: | 1 | Requires 
Target
X3 [L62473-L62501] ( )
, | , | , | , | , | , | , | {
X4 [L62502-L62522] ( $14P = TRUE )
X5 [L62523-L62572] ( )
, | Save | target spell | { | { | } | 0-100 | -
X6 [L62573-L62581] ( )
X7 [L62582-L62628] ( $44O , $44K , $901 = $STR16951 )
mas | mas | gfb | ue | mortn | morts | morte | mortw
CQ [L62629-L62650] ( )
CW [L62651-L62978] ( $44U = NULL )
, | { | { | } | 0-100 | - | * | MagicShooter: Friend has low hp temp stop attacking.
CE [L62979-L63052] ( )
<PARSED TEXT FOR PAGE: 77 / 88>
skillTrainer | , | Slime
CR [L63053-L63061] ( $92Y )
CT [L63062-L63073] ( $92Y , $92U = 0 )
CY [L63074-L63085] ( )
CU [L63086-L63325] ( )
skillTrainer | delay | , | Targeting: Failed to find attaking creature ( | ) in all creatures list. | Targeting: Creature 
pos.z ( | ) is different than character pos.z ( | )
CI [L63326-L63337] ( $56 , $37W )
~ | ~
CO [L63338-L63349] ( $83W = TRUE )
Targeting | Targeting
CP [L63350-L63353] ( )
CA [L63354-L63359] ( )
if stuck
CS [L63360-L63368] ( $83W = TRUE )
Targ if stuck | Targeting
CD [L63369-L63379] ( $132 = 0 , $462 = TRUE )
skillTrainer
CF [L63380-L63392] ( $48S = $STR17027 )
follow | none | follow | 2sqm | 3sqm | 4sqm | diagonal | diagonal 3sqm
CG [L63393-L63395] ( )
CH [L63396-L63477] ( )
follow | dword | byte[150] | 0x | 0x | follow | stand near | 1
CJ [L63478-L63485] ( $49K )
CK [L63486-L63495] ( $84A , $92J )
CL [L63496-L63523] ( )
dword | dword | dword | byte
CZ [L63524-L63539] ( $12W )
dword | dword | byte
<PARSED TEXT FOR PAGE: 78 / 88>
CX [L63540-L63604] ( )
Looter: set Creature looted due time | byte | ushort | ushort | byte | byte | ushort | ushort
CC [L63605-L63624] ( $12W )
CV [L63625-L63627] ( )
CB [L63628-L63638] ( )
CN [L63639-L63647] ( $122 )
CM [L63648-L63895] ( $92M = TRUE , $92_ = TRUE )
Looter: return true due max use tries | autoLoot | \d+ | \d+ | \d+ | \d+ | \d+ | \d+
C_ [L63896-L63903] ( $37W )
the | demonic | dead | slain | dissolved | remains | elemental | split
C0 [L63904-L64063] ( )
Looter: failed to get skinning creature. | , | hunters knife | mythril knife | dark knife | skinpeeler | skinning knife | 
obsidian knife
C1 [L64064-L64075] ( )
C2 [L64076-L64234] ( )
bool | Looter: ignore due outOfScreen | Looter: ignore due ladder | Looter: ignore due not openable or trashed: | 
Looter: ignore due max Open Tries | Looter: ignore with <unknown> reason
C3 [L64235-L64243] ( $83W = TRUE )
C4 [L64244-L64247] ( )
C5 [L64248-L64252] ( )
C6 [L64253-L64259] ( $0A , $121 )
wchar Text[1000] | wparam | struct* | Text
C7 [L64260-L64262] ( $0A )
VQ [L64263-L64265] ( $0A )
VW [L64266-L64268] ( $0A , $121 , $93P = FALSE )
VE [L64269-L64274] ( $0A , $121 , $13I )
VR [L64275-L64277] ( $0A , $13I , $121 = + 4294967295 )
VT [L64278-L64280] ( $0A , $121 )
VY [L64281-L64283] ( $0A , $83W )
VU [L64284-L64286] ( $0A )
<PARSED TEXT FOR PAGE: 79 / 88>
VI [L64287-L64289] ( $0A , $82E )
VO [L64290-L64292] ( $0A , $46J , $87P )
VP [L64293-L64298] ( $0A , $2E , $0U = 0 , $2R = 0 , $2T = 0 , $2Y = $STR17135 , $2U = $STR17136 , $2I = $STR17137 )
wparam | lparam | lresult | user32.dll | SendMessageW | hwnd | uint
VA [L64299-L64349] ( )
Found Scripts: | 1 | Execute Script | Refresh List | Source | Running Scripts: | 1 | Kill Script
VS [L64350-L64360] ( )
VD [L64361-L64364] ( $82G )
VF [L64365-L64377] ( )
*.* | .lua
VG [L64378-L64414] ( $434 = NULL , $93D = FALSE )
.lua | Successully load | .lua
VH [L64415-L64465] ( $434 = NULL , $93G = TRUE )
VJ [L64466-L64490] ( )
Successully reload scripts
VK [L64491-L64504] ( )
.lua
VL [L64505-L64510] ( )
VZ [L64511-L64518] ( $105 , $93L = FALSE )
 | | Lua error: | Lua Scripter
VX [L64519-L64522] ( $93Z )
Lua:
VC [L64523-L64589] ( $100 )
LUA ERROR: Failed to execute script, re-open bot as admin. | coroutineOneLineStatus | coroutineOneLineHandle | 
function sleep(deltaTime) | if coroutineOneLineHandle == 0 then return false end | coroutineOneLineTime 
= os.clock() + (deltaTime / 1000) | coroutine.yield() | end
VV [L64590-L64901] ( $82G , $93N = NULL )
lua51-x64.dll or lua5.1-x64.dll missing! Re-download Rifbot. | lua51.dll or lua5.1.dll missing! Re-download Rifbot. | 
Failed to open lua51-x64.dll! Download and install Microsoft Visual C++ 2010 Redistributable Package (x64) | Failed to
open lua51.dll! Download and install Microsoft Visual C++ 2010 Redistributable Package (x86) | Failed to create a new
state for lua script. Run program as Administrator. | Failed to load Lua. Run program as Administrator. | Error | :
<PARSED TEXT FOR PAGE: 80 / 88>
VB [L64902-L64925] ( )
/ | \ | : | * | " | ? | < | >
VN [L64926-L64932] ( )
Line: | GetCurrentLine | Line:
VM [L64933-L64938] ( $13A )
V_ [L64939-L64942] ( $13A )
V0 [L64943-L64947] ( $13A )
V1 [L64948-L64951] ( $13A )
V2 [L64952-L64955] ( $13A )
V3 [L64956-L64960] ( $13A )
V4 [L64961-L64965] ( $13A )
V5 [L64966-L64971] ( $13A )
V6 [L64972-L64975] ( $13A )
V7 [L64976-L64980] ( $13A )
BQ [L64981-L64984] ( $13A )
BW [L64985-L64988] ( $13A )
BE [L64989-L64992] ( $13A )
BR [L64993-L64996] ( $13A )
BT [L64997-L65001] ( $13A )
BY [L65002-L65006] ( $13A )
BU [L65007-L65010] ( $13A )
BI [L65011-L65014] ( $13A )
BO [L65015-L65019] ( $13A )
BP [L65020-L65024] ( $13A )
BA [L65025-L65190] ( $13A )
healing | uh | mf | mp | mr | lf | friends | sio
BS [L65191-L65194] ( $13A )
BD [L65195-L65200] ( $13A )
BF [L65201-L65206] ( $13A )
<PARSED TEXT FOR PAGE: 81 / 88>
.txt | .txt
BG [L65207-L65211] ( $13A )
BH [L65212-L65221] ( $13A )
BJ [L65222-L65229] ( $13A )
BK [L65230-L65233] ( $13A )
BL [L65234-L65240] ( $13A )
BZ [L65241-L65244] ( $13A )
BX [L65245-L65250] ( $13A )
Lua
BC [L65251-L65255] ( $13A )
BV [L65256-L65258] ( $13A )
BB [L65259-L65261] ( $13A )
BN [L65262-L65280] ( $13A )
pid | hwnd | base | time | start
BM [L65281-L65288] ( $13A )
sdsf:
B_ [L65289-L65295] ( $13A )
B0 [L65296-L65301] ( $13A )
0x
B1 [L65302-L65312] ( $13A )
B2 [L65313-L65323] ( $13A )
B3 [L65324-L65328] ( $13A )
B4 [L65329-L65334] ( $13A )
B5 [L65335-L65339] ( $13A )
B6 [L65340-L65360] ( $13A )
x | y | z
B7 [L65361-L65373] ( $13A )
x | y
NQ [L65374-L65385] ( $13A )
<PARSED TEXT FOR PAGE: 82 / 88>
x | y
NW [L65386-L65390] ( $13A )
NE [L65391-L65404] ( $13A )
NR [L65405-L65444] ( )
attempt to call a nil value | .lua: | .lua line | none:cdecl | lua_close | handle
NT [L65445-L65448] ( $13A )
NY [L65449-L65452] ( $13A )
NU [L65453-L65456] ( $13A )
NI [L65457-L65460] ( $13A )
NO [L65461-L65464] ( $13A )
NP [L65465-L65468] ( $13A )
NA [L65469-L65472] ( $13A )
NS [L65473-L65476] ( $13A )
ND [L65477-L65480] ( $13A )
NF [L65481-L65484] ( $13A )
NG [L65485-L65488] ( $13A )
NH [L65489-L65502] ( $13A )
x | y | z
NJ [L65503-L65520] ( $13A )
x | y | z
NK [L65521-L65524] ( $13A )
NL [L65525-L65528] ( $13A )
NZ [L65529-L65532] ( $13A )
NX [L65533-L65536] ( $13A )
NC [L65537-L65541] ( $13A )
NV [L65542-L65553] ( $13A )
id | count
NB [L65554-L65557] ( $13A )
NN [L65558-L65562] ( $13A )
<PARSED TEXT FOR PAGE: 83 / 88>
NM [L65563-L65566] ( $13A )
N_ [L65567-L65570] ( $13A )
N0 [L65571-L65574] ( $13A )
N1 [L65575-L65578] ( $13A )
N2 [L65579-L65582] ( $13A )
N3 [L65583-L65588] ( $13A )
N4 [L65589-L65594] ( $13A )
N5 [L65595-L65601] ( $13A )
N6 [L65602-L65607] ( $13A )
N7 [L65608-L65616] ( $13A )
WQQ [L65617-L65623] ( $13A )
WQW [L65624-L65659] ( $13A )
skull | party | attack | addr | direction | alive | hpperc | z
WQE [L65660-L65694] ( $13A )
skull | party | attack | addr | direction | alive | hpperc | z
WQR [L65695-L65702] ( $13A )
WQT [L65703-L65712] ( $13A )
WQY [L65713-L65717] ( $13A )
WQU [L65718-L65721] ( $13A )
WQI [L65722-L65726] ( $13A )
WQO [L65727-L65736] ( $13A )
WQP [L65737-L65748] ( $13A )
WQA [L65749-L65752] ( $13A )
WQS [L65753-L65757] ( $13A )
WQD [L65758-L65764] ( $13A )
WQF [L65765-L65768] ( $13A )
WQG [L65769-L65772] ( $13A )
WQH [L65773-L65776] ( $13A )
WQJ [L65777-L65780] ( $13A )
<PARSED TEXT FOR PAGE: 84 / 88>
WQK [L65781-L65787] ( $13A )
WQL [L65788-L65800] ( $13A )
cont | eq | FFFF | map
WQZ [L65801-L65806] ( $13A )
WQX [L65807-L65812] ( $13A )
WQC [L65813-L65818] ( $13A )
WQV [L65819-L65824] ( $13A )
WQB [L65825-L65829] ( $13A )
WQN [L65830-L65870] ( $13A )
id | name | x | y | z | hpperc | alive | direction
WQM [L65871-L65875] ( $13A )
WQ_ [L65876-L65917] ( $13A )
x | y | z | amount | items | id | count
WQ0 [L65918-L65934] ( $13A )
id | count | stack
WQ1 [L65935-L65952] ( $13A )
return { | {id = | , count = | , stack = | , x = | , y = | , z = | },
WQ2 [L65953-L65965] ( $13A )
WQ3 [L65966-L65976] ( $13A )
WQ4 [L65977-L65997] ( $13A )
id | count
WQ5 [L65998-L66007] ( $13A )
WQ6 [L66008-L66013] ( $13A )
WQ7 [L66014-L66036] ( $13A )
index | id | name | size | amount
WWQ [L66037-L66040] ( $13A )
WWW [L66041-L66088] ( $13A )
id | count | index | items | id | count
WWE [L66089-L66106] ( $13A )
<PARSED TEXT FOR PAGE: 85 / 88>
WWR [L66107-L66137] ( $13A )
WWT [L66138-L66160] ( $13A )
WWY [L66161-L66169] ( $13A )
WWU [L66170-L66203] ( $13A )
WWI [L66204-L66229] ( $13A )
WWO [L66230-L66240] ( $13A )
WWP [L66241-L66251] ( $13A )
WWA [L66252-L66255] ( $13A )
WWS [L66256-L66260] ( $13A )
WWD [L66261-L66271] ( $13A )
speaker | message
WWF [L66272-L66277] ( $13A )
WWG [L66278-L66296] ( $13A )
WWH [L66297-L66315] ( $13A )
WWJ [L66316-L66334] ( $13A )
WWK [L66335-L66352] ( $13A )
WWL [L66353-L66358] ( $13A )
WWZ [L66359-L66367] ( )
libOnLabel | routine
WWX [L66368-L66389] ( )
attempt to call a nil value | .lua: | .lua line
WWC [L66390-L66427] ( )
speaker | message | level | channel | mode | attempt to call a nil value | .lua: | .lua line
WWV [L66428-L66465] ( )
message | mode | attempt to call a nil value | .lua: | .lua line
WWB [L66466-L66499] ( )
message | x | y | z | attempt to call a nil value | .lua: | .lua line
WWN [L66500-L66512] ( $13A )
WWM [L66513-L66518] ( $13A )
<PARSED TEXT FOR PAGE: 86 / 88>
WW_ [L66519-L66522] ( $13A )
WW0 [L66524-L66542] ( $13A )
AutoIt.Error | WinHttp.WinHttpRequest.5.1 | post | get
WW1 [L66543-L66553] ( $13A )
WW2 [L66554-L66564] ( $13A )
WW3 [L66565-L66590] ( $13A )
status | data | status | data
WW4 [L66591-L66593] ( $94V , $81V )
WW5 [L66594-L66596] ( $13M , $81V )
WW6 [L66597-L66604] ( $81V )
,
WW7 [L66605-L66613] ( $81V )
WEQ [L66614-L66622] ( $81V )
WEW [L66623-L66833] ( $94B = FALSE )
\Settings\ | example | \ | .txt | Save Settings | \Settings | Text (*.txt)| | \Startup Settings.txt
WEE [L66834-L66841] ( $82A , $81V , $82E )
WER [L66842-L66861] ( $82A , $81V , $94_ , $82E )
4 | none | 1 | follow | |
WET [L66862-L66866] ( $82A , $81V , $82E )
WEY [L66867-L66872] ( BYREF $13M , $81V , $940 , $82E , $15T = TRUE )
WEU [L66873-L66878] ( BYREF $12C , $81V , $940 , $82E , $11A = FALSE )
WEI [L66879-L66894] ( $81V , $82E )
, | ,
WEO [L66895-L66909] ( $81V , $82E )
#### Settings ####
WEP [L66910-L66922] ( $81V , $82E )
#### Settings II ####
WEA [L66923-L67152] ( $434 = NULL , $941 = FALSE )
\ | Load Settings | \Settings | Text (*.txt) | | | \Settings\ | Load Settings | Rifbot can't load old version settings.
<PARSED TEXT FOR PAGE: 87 / 88>
WES [L67153-L67182] ( )
Startup Settings.txt | \Settings | *.* | Error 09 | Missing settings directory! | Make sure that you have all files of 
Rifbot. | .txt | \Settings\
WED [L67183-L67185] ( )
WEF [L67186-L67188] ( )
WEG [L67189-L67203] ( )
-s | -s | .txt | .txt
WEH [L67206-L67285] ( )
malvera
WEJ [L67286-L67513] ( )
malvera
WEK [L67514-L67517] ( )
WEL [L67518-L67523] ( $16S , $132 , BYREF $52Q )
WEZ [L67524-L67531] ( $95Q , $95W , $95E , $95R )
WEX [L67543-L67548] ( $145 )
int64 time; | time | ntdll.dll | dword | ZwDelayExecution | int | ptr
14. Projeto limpo para nossa 
implementação
profiles/
 BuildDetector
 Olders_3_3_273
memory/
 ModuleResolver
 Memory
sdk/
 Position Creature LocalPlayer Item Tile Map Container
actions/
 Combat Movement Interaction
bot/
 Conditions Healing Targeting Walker Looter ContainerManager
runtime/
 Dispatcher
api/
 BotApi
<PARSED TEXT FOR PAGE: 88 / 88>
tests/
 ProfileTests SnapshotTests ActionTests
15. Regras para não quebrar em updates
 Offsets e RVAs pertencem ao BuildProfile, nunca ao código de Targeting/Looter/Healing.
 Um campo vindo somente de profile legado não é promovido automaticamente ao profile atual.
 Campos desconhecidos permanecem unknown até callers/uso confirmarem a semântica.
 Ações nativas ficam atrás de uma interface e são executadas no contexto/thread apropriado.
 Snapshots de leitura são separados das ações que alteram o estado do cliente.
 Cada build ganha fingerprint e teste de compatibilidade antes de habilitar ações.
16. Arquivos de apoio
O pacote complementar contém: fonte reconstruído, todas as strings decodificadas, inventário completo de funções, 
globais numéricas, profiles extraídos e índices. Use o livro para entender a arquitetura e os CSVs/fonte para pesquisa 
pontual.