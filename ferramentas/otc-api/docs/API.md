# Contrato da pipe

`\\.\pipe\OTCApi` — JSON-lines (UTF-8, uma mensagem por linha, `\n`). Offsets e RVA **nunca** vazam daqui. O app (outro repo) só fala este contrato. Layout de memória: `configs/calibrations/README.md` e `{buildId}.json`.

Contrato da **etapa 1** (P0–P5, fechada): leitura (self, inventário, containers, criaturas, mapa) e ações (`say`, `step`, `stop`, `logout`, `walkTo`, `useItem`, `moveItem`, `attack` / `follow` e cancels). Features novas entram neste arquivo.

Uma conexão de cada vez **por instância da DLL**. Ao conectar, a DLL reenvia o snapshot atual (`dllStatus`, `playerStats`, `inventory`, containers abertos, criaturas visíveis).

Por compatibilidade, o nome padrão continua sendo `\\.\pipe\OTCApi`. Para controlar vários jogos simultaneamente, defina `OTC_API_PIPE_NAME` no ambiente do processo do jogo antes da injeção. A DLL usará esse nome, permitindo uma pipe por processo, por exemplo:

```text
OTC_API_PIPE_NAME=OTCApi_21716
\\.\pipe\OTCApi_21716
```

O valor pode ser informado com ou sem o prefixo `\\.\pipe\`. O consumidor deve associar o nome ao PID do jogo que ele controla; não há roteamento global nem mistura de eventos entre instâncias. Monitor e calibradores conectados à mesma pipe continuam compartilhando aquela instância e devem ser executados um por vez.

O injector configura a pipe antes de carregar a DLL:

```powershell
OTCInjector.exe --pid 21716 --pipe-name OTCApi_21716
```

Para um jogo já aberto, o injector publica essa configuração em uma área nomeada privada do PID; a DLL lê a configuração durante a inicialização e confirma a leitura por um evento nomeado antes que o injector libere o mapping. Assim, definir a variável somente no ambiente do injector não é necessário. Esse fluxo foi validado com dois clientes simultâneos.

Os perfis aprovados de `configs/calibrations/` são incorporados à `OTCApi.dll` durante o build. Um JSON válido em `<DLL>/calibrations/{buildId}.json` continua como override gravável para calibração; se faltar ou for inválido, a DLL usa a cópia embutida do mesmo `buildId`. Assim, um consumidor final precisa somente da DLL recompilada, sem arquivo de perfil externo.

No Windows, o consumidor precisa conseguir abrir a pipe criada dentro do processo do jogo. Se o cliente está como administrador, rode injector, calibrador e Monitor no mesmo nível; `EPERM` normalmente é diferença de integridade, não erro do JSON.

## Envelope

Evento (DLL → app):

```json
{"type":"event","event":"playerStats","data":{}}
```

Comando (app → DLL):

```json
{"action":"say","text":"exura","requestId":"1"}
```

`requestId` é opcional. Se vier, a DLL ecoa no evento de resposta (`action`, `state`, `pong`, `calibration`).

Ação desconhecida: a DLL **não** responde (só log). `getState` com `what` desconhecido responde `state` com `data.error`.

## Eventos

| `event` | Origem | `data` |
|---------|--------|--------|
| `dllStatus` | inject / connect / ready | `phase`, `message`, `buildId`, `architecture`, `hasChain`, `statsBase`, `profileFound`, `calibrationAvailable`, `compatibility` |
| `playerStats` | P1 poll ~250 ms (snapshot **inteiro** a cada ciclo) e no connect | objeto self |
| `inventory` | P1 poll ~250 ms (10 slots a cada ciclo) e no connect | array |
| `containerOpen` / `containerUpdate` / `containerClose` | P2 poll ~250 ms (**diff**) | um container |
| `creatureAppear` / `creatureUpdate` / `creatureDisappear` | P3 poll ~250 ms (**diff**) | criatura (disappear: `id`,`name`) |
| `action` | resposta P4 | `ok`, `action`, `via`?, `error`?, `follow`?, `requestId`? — `look` também ecoa o alvo (`kind`,`id`,`x,y,z`…); `setLight` ecoa `enabled` e `intensity` |
| `look` | cliente fez look (Shift+click ou ação `look`) | alvo: item id e/ou SQM `x,y,z` |
| `corpse` | acompanhamento explícito de alvo | `status`, `corpseId`, `creatureId`, `name`, `x,y,z`, `itemId`?, `evidence`? |
| `monsterEnd` | P3 passivo, sem alvo de ataque | `creatureId`, `name`, `x,y,z`, `hpperc`, `deathObserved`, `positionSource`, `reason` |
| `state` | resposta `getState` | `what`, `data`, `requestId`? |
| `pong` | resposta `ping` | `ok: true` |
| `calibration` | wizard `tools/` | status + `ok`? — **não** é API de script |

Na calibração P3, `--hpperc 0..100` é opcional e deve representar o valor exato
visível. Sem essa opção, o scanner não presume 100. Uma leitura apenas dentro da
faixa não valida o campo, pois enums próximos também podem conter 0–3.

## Status da DLL

Evento `dllStatus`:

| `phase` | Significado |
|---------|-------------|
| `loaded` | DLL no processo, pipe à espera |
| `connected` | um cliente na pipe |
| `needCalibration` | não há perfil aplicável, o personagem ainda não está no mapa, ou a chain não restaurou; veja `message` |
| `scanning` | wizard está procurando LocalPlayer; o snapshot anterior não deve ser tratado como pronto |
| `ready` | memória do player viva (`playerStats`) |

Campos: `phase`, `message` (texto de diagnóstico), `buildId`, `architecture`
(`"x86"` ou `"x64"` do processo que carregou a DLL), `hasChain` (perfil P1) e
`statsBase` (bool: âncora LocalPlayer achada). Os campos aditivos
`profileFound` e `calibrationAvailable` são verdadeiros quando a DLL carregou
um perfil válido para o `buildId`, mesmo sem personagem no mapa. Isso permite
ao app decidir compatibilidade ainda na tela de login.

`compatibility` é um resumo estável para o app:

| valor | significado |
|-------|-------------|
| `compatible` | perfil válido e LocalPlayer restaurado |
| `waiting_for_player` | perfil válido, aguardando o personagem entrar no mapa |
| `not_calibrated` | nenhum perfil válido para este `buildId` |

O valor é diagnóstico de disponibilidade da build; para automação de memória,
continue exigindo `phase:"ready"` e um `playerStats` válido.

`buildId` é o fingerprint do PE em disco. O app não escolhe servidor nem envia offsets: uma build diferente recebe outro perfil. Para automação, só considere o player disponível após `phase:"ready"` e um `playerStats` válido; `connected` confirma apenas a pipe.

Os campos de stats usados para validar a restauração são de layout, não de
identidade do personagem. Em particular, `mana`/`maxMana` podem ser `0/0` em
um personagem ou durante a reconstrução do `LocalPlayer`; isso não invalida o
perfil da build. A recuperação continua exigindo posição, inventário e demais
evidências estruturais antes de aceitar um candidato.

Se uma cadeia P1 salva falhar, a DLL pode recuperar o LocalPlayer pela referência
Game identificada nos metadados do cliente, reutilizando o layout calibrado.
Essa recuperação exige posição, inventário e objeto válidos, candidato único e
duas observações consecutivas. Ela salva somente a cadeia corrigida no perfil
existente; não executa ações nem recalibra campos. Sem evidência suficiente,
permanece em `needCalibration`. `message` informa recuperação ou falha de gravação.

Mensagens dessa rota: `LocalPlayer recovered; profile chain repaired` quando salva;
`LocalPlayer recovered; profile save failed (check DLL folder permissions)` quando
recupera em memória, mas não consegue persistir. Depois da recuperação, outro restart
deve retornar `LocalPlayer live` por resolução direta. Não há novo comando público.

## Disponibilidade por build

O contrato é estável, mas uma ação pode responder `ok:false` quando o perfil daquela build ainda não demonstrou seu RVA/ABI. Isso permite usar toda a leitura no Monitor sem fingir que uma ação perigosa está calibrada.

| Build/perfil | Estado observado |
|--------------|-----------------|
| KarmaOT_GL `pe-407f0b0e-…` | baseline P1–P5 e ações da tabela validadas ao vivo |
| DeusOT `pe-a6b7516c-6a8eddb3-01ba3000-0121a200-014c` | player, posição/states, inventário, containers, criaturas e mapa validados; perfil pronto para Monitor |
| mesmo DeusOT, `attack`/`follow` | alvo não nulo raw/intrusivo ainda não suportado; o perfil distribuído mantém os RVAs em `0` e a chamada falha fechada |
| Valantis `pe-61beeb3a-…` | P1 recuperado e restart direto validados; P2/P3/P5 com sucesso de calibração no log; validação completa dessas etapas ainda pendente |

As linhas históricas não garantem JSON incluído no pacote: o reset local de 02/09
retirou os perfis antigos e o catálogo fonte está sem JSON. Ver
[registro de alterações e limites](CALIBRATION-NOTES-2026-09-02.md).

O build DeusOT `pe-a9376da6-6a885df0-01b9c000-01215000-014c` é um PE anterior com somente P1 histórico; não é fallback para a build atual. Layout e runbook: `configs/calibrations/README.md`.

## Coordenadas do monstro: `monsterEnd`

Modo simples, passivo, sem `watch`, alvo de ataque, item ID, P5 ou ações:

```powershell
node tools/pipe_test.js --only deaths
```

A DLL acompanha todos os monstros visíveis lidos pelo P3. Ao observar HP 0 ou
o desaparecimento de um monstro previamente vivo, emite:

```json
{"type":"event","event":"monsterEnd","data":{"creatureId":1073765702,"name":"Sheep","x":32060,"y":32210,"z":7,"hpperc":0,"deathObserved":true,"positionSource":"current","reason":"zero-hp"}}
```

`deathObserved:true` significa HP 0 observado. `positionSource:"current"` usa
a posição válida dessa amostra; `positionSource:"last-seen"` usa a última
coordenada válida, quando o objeto morto ainda existe na coleção com posição
inválida. Objetos removidos (`65535,65535,255`) são excluídos da lista visível,
sem invalidar a leitura de todas as outras criaturas. HP 0 nesse objeto só
gera evento se a mesma criatura foi acompanhada viva anteriormente.
Se não houver amostra com HP 0, `deathObserved:false`, `reason:"disappeared"`
e `hpperc`/coordenadas são os últimos valores observados, com
`positionSource:"last-seen"`. Nesse caso, **não há
confirmação de morte**: o monstro pode ter saído de alcance, teleportado ou
mudado de andar. Não comprova autoria do kill nem localização atual de um corpo.
O poll pode perder o movimento final antes do desaparecimento.

Requer P1 e P3 calibrados para o buildId atual. Não depende de `targetCreatureId`.
Desconexão do personagem, mudança de andar ou falha de leitura reiniciam a
referência sem produzir mortes. Primeira amostra é referência; cadáver/HP 0
já presente nela não produz evento. HP 0 seguido de desaparecimento emite uma
vez. Monstro que reaparece vivo pode gerar um novo evento no futuro.

`getState what=monsterEnd` retorna o último evento (`{}` antes do primeiro ou
após invalidar a referência). `--only deaths` consulta esse estado ao conectar,
mostra o snapshot P3 inicial, os eventos `monsterEnd` e `dllStatus`.
O modo anterior `--only corpse` também mostra `monsterEnd` automaticamente;
seus comandos explícitos continuam disponíveis por compatibilidade.

Validação ao vivo em 08/09/2026, reportada pelo usuário: Valantis x86,
`pe-165c7c71-6a9f25d8-00b48000-006ce200-014c`, Sheep 1073765702,
evento exatamente como o exemplo acima após matar o monstro. Isso valida a
emissão de coordenadas com HP 0 e `positionSource:current` nessa build.
No connect, `state what=monsterEnd` devolveu um desaparecimento anterior da
mesma Sheep com HP 100: esse snapshot é histórico e não deve ser interpretado
como nova morte, nem como posição atual; a criatura pode reaparecer viva.
A variante de morte `last-seen` permanece coberta por testes automatizados;
regressão Karma e abertura de corpos não foram validadas por esse teste.

## Corpo do alvo: `watchCorpse`, `corpse`, `openCorpse`

Teste interativo na mesma conexão da pipe, com a DLL atual injetada:

```powershell
node tools/pipe_test.js --only corpse
```

No terminal, digite apenas `watch` enquanto
ataca o monstro vivo. O tool consulta `targetCreatureId` e envia `watchCorpse`.
O item ID é identificado pela DLL, comparando o tile antes e depois da morte.
O filtro antigo continua opcional: `watch 1234 5678`. Após `corpse` com `status:available`, digite
`open`: usa o último `corpseId` recebido. Se retornar `walking`, espere chegar
ao lado do corpo e digite `open` novamente. Também aceita `open 1` com token
explícito, `state` para consultar o acompanhamento e `quit` para desconectar.
Sem `watch`, o modo apenas observa; não ataca nem registra alvos automaticamente.
O filtro mostra respostas, estado, eventos de corpo, alvo, look e `containerOpen`.
Shift+click em um corpo existente pode fornecer seu item ID pelo evento `look`.
Feche Monitor/calibradores antes: a pipe aceita um consumidor por vez.

Fluxo sob comando do consumidor: antes da morte, registrar o monstro atacado;
receber o SQM do corpo; pedir aproximação e, ao chegar, pedir abertura.
Não coleta loot nem escolhe novos alvos. Requer P1/P3/P5 e ações calibradas.

```json
{"action":"watchCorpse","requestId":"watch-1"}
```

`creatureId` omitido seleciona o alvo atual do player no momento do comando.
`corpseItemIds` é opcional; quando informado, contém de 1 a 32 client item IDs
e preserva o filtro explícito anterior. Omitido, habilita identificação automática.
O monstro deve estar visível, vivo e ser o `targetCreatureId` atual. Uma única
observação fica ativa; novo registro substitui o anterior. Expira em 60 segundos.
`watchCorpse` responde `action` com `ok`; nenhum movimento ocorre nessa etapa.

Poll de 250 ms acompanha a última posição e os itens preexistentes nesse tile.
A amostra lê o mapa primeiro e depois relê as criaturas diretamente pela coleção
P3 calibrada. Não usa o cache P3 para atualizar o tile de referência: um cache
anterior à morte poderia incorporar o corpo como item preexistente.
Falha de leitura da coleção não equivale a desaparecimento do monstro.
Quando o alvo some ou chega a HP 0, há até 2 segundos para encontrar exatamente
um item novo no último SQM observado. No modo automático, os itens anteriores
devem continuar iguais e na mesma ordem; só uma inserção de ID novo, com count
0 ou 1, é aceita, persistindo em dois polls. Remoções, transformações, pilhas e
múltiplas inserções são ambíguas e não produzem candidato. Tile sem baseline,
corpo preexistente, múltiplos candidatos, troca de andar ou perda de player não
produzem corpo utilizável. Um desaparecimento isolado não sinaliza morte.

```json
{"type":"event","event":"corpse","data":{"status":"available","corpseId":1,"creatureId":1073793728,"name":"Rat","x":32340,"y":32224,"z":7,"itemId":1234,"evidence":"target-disappeared-new-item"}}
{"action":"getState","what":"corpse","requestId":"state-1"}
{"action":"openCorpse","corpseId":1,"requestId":"open-1"}
```

O estado é `{}` antes do primeiro registro; depois contém `status` (`watching`,
`available`, `walking`, `useRequested`, `expired`), `corpseId`, `creatureId`,
`name`, `x,y,z`, `identification` (`automatic` ou `filtered`) e, quando encontrado,
`itemId` e `evidence`.
`getState what=corpse` permite recuperar o estado após reconectar.
`corpseId` é um token local à DLL; não deve ser reutilizado após reinjeção.
O evento `corpse` é emitido nas transições; o corpo disponível expira em 60 s.
Em `expired`, `reason` distingue `watch timeout`, `corpse timeout`,
`player unavailable`, `floor changed` ou `detection timeout`.
`diagnostic` informa a última barreira de detecção (`no new item`,
`ambiguous tile change`, `candidate not stable`, `baseline unavailable`,
`map tile unavailable`, `creatures unavailable`). `baselineItems` e
`observedItems` mostram `{id,count}` do último tile comparado, sem ponteiros.

`openCorpse` revalida token, player, andar e presença única do item. Distância
Chebyshev maior que 1 chama `walkTo` e responde `status:"walking"`, com o SQM.
O consumidor acompanha `playerStats` e repete `openCorpse` ao chegar a até 1 SQM.
A repetição durante o percurso não reinicia o autoWalk. Sem caminho retorna erro;
percurso interrompido pode ser retomado por `walkTo` explícito. Ao alcance, usa o
item somente se o top-use thing ainda tiver o ID esperado; item coberto falha.
Sucesso responde `status:"useRequested"`. Repetir após isso é idempotente.
Confirme a abertura pelo evento P2 `containerOpen`: `ok:true` confirma a chamada
do cliente, não a abertura aceita pelo servidor. Não há retry automático de use.

A correlação identifica um **candidato a corpo do alvo atacado**, não autoria do
último golpe nem propriedade do loot. A memória disponível não comprova esses
fatos; outro jogador pode finalizar o monstro. Mortes entre polls, deslocamento
no último instante e corpos fora da janela P5 podem não ser detectados.
O modo automático usa correlação temporal e espacial, não um atributo de tipo
`isCorpse`: um item solto no mesmo instante/SQM pode satisfazer a correlação.
`available` indica um candidato; confirme a abertura pelo P2. Nenhuma tabela de
IDs por servidor é embutida e não é necessário matar um exemplar antes.
`evidence` é `target-zero-hp-new-item` ou `target-disappeared-new-item`.

Erros incluem `bad creatureId`, `bad corpseItemIds`, `target not attackable`,
`map tile unavailable`, `corpse unavailable`, `corpse changed`, `player unavailable`,
além dos erros existentes de `walkTo`/use. Nenhum ABI/RVA novo é presumido.
Implementação exige validação live por build; build/testes não a substituem.
Nesta versão, abertura exige `ItemPtr` compartilhado de 8 bytes calibrado;
builds com ponteiros raw de 4 bytes retornam `corpse use ABI not supported`.
Detecção e localização continuam disponíveis com o layout P5 da build.

## ping

```json
{"action":"ping","requestId":"0"}
```

```json
{"type":"event","event":"pong","data":{"ok":true,"requestId":"0"}}
```

## getState

```json
{"action":"getState","what":"player","requestId":"1"}
```

Resposta: evento `state`.

| `what` | `data` |
|--------|--------|
| `player` (omisso = este) | objeto `playerStats` |
| `inventory` | array de 10 slots |
| `containers` / `container` | array das janelas abertas |
| `creatures` / `creature` / `battle` | array das criaturas da lista P3 |
| `map` / `tiles` / `tile` | array de tiles P5 (mesmo `z`, raio Chebyshev do perfil) |
| `look` | último look (`{}` se ainda não houve) |
| `corpse` | acompanhamento do corpo (`{}` antes do primeiro registro) |
| `monsterEnd` | último evento de coordenadas (`{}` antes do primeiro ou após invalidar a referência) |
| `calibration` | status do wizard (não usar no app) |
| outro | `{"error":"unknown what"}` |

```json
{"type":"event","event":"state","data":{"what":"creatures","data":[{"id":1073793728,"name":"Rat"}],"requestId":"1"}}
```

## Self (P1)

Evento `playerStats` / `getState what=player`. Poll ~250 ms: a DLL **reenvia o snapshot** a cada ciclo (não é só diff). No connect também.

| Campo | Tipo | |
|-------|------|--|
| `health`, `maxHealth` | int | |
| `mana`, `maxMana` | int | |
| `level`, `levelPercent` | int | `%` do level 0–100 |
| `experience` | uint64 | |
| `capacity` | int | cap **livre** (já com `capacityScale` do perfil) |
| `totalCapacity` | int | cap total |
| `magicLevel`, `soul` | int | |
| `stateFlags` | uint | bits crus |
| `conditions` | string[] | nomes dos bits ligados |
| `position` | `{x,y,z}` | presente com o char no mapa. Offset P1 do perfil; se falhar, tile do objeto LocalPlayer (layout P3) e o offset vivo fica em cache. Ausente só sem âncora/layout |
| `name` | string | nome do personagem, quando o layout P3 do perfil possui o nome do `LocalPlayer` |
| `direction` | `north` / `east` / `south` / `west` | direção atual do personagem, quando o perfil calibrou o offset correspondente |
| `targetCreatureId` | uint32 | Creature id P3 que o player está atacando. `0` = sem alvo ou leitura não calibrada para a build |

`totalCapacity`, `magicLevel` e `soul` permanecem no contrato e valem `0` quando o respectivo campo não existe ou ainda não foi demonstrado para aquele `buildId`. O perfil omite o offset desconhecido em vez de apontá-lo para outro atributo.

`conditions` possíveis: `poison`, `fire`, `energy`, `drunk`, `manaShield`, `paralyze`, `haste`, `battle`, `drowning`, `freezing`, `dazzled`, `cursed`, `partyBuff`, `pzBlock`, `pz`, `bleeding`, `hungry`.

Quando o alvo muda, a DLL também emite `{"type":"event","event":"attackTarget","data":{"creatureId":1073793728}}`. Cancelar ou perder o alvo emite `creatureId: 0`. No connect, o estado atual é reenviado. O ponteiro nunca sai na pipe; a DLL lê o membro `Game::m_attackingCreature` no offset calibrado da build e resolve o `id` pelo layout P3.

## Inventário (P1)

Evento `inventory` / `getState what=inventory`: **sempre 10 entradas**, slots **1–10**. `id` 0 = vazio. Poll ~250 ms (snapshot completo a cada ciclo, como `playerStats`).

Ordem dos slots: `1=head`, `2=necklace`, `3=backpack`, `4=armor`,
`5=right-hand`, `6=left-hand`, `7=legs`, `8=feet`, `9=ring`, `10=ammo`.
Hints de calibração e ações `--useInv`/`--moveInv` usam essa numeração.

| `slot` | Equip |
|--------|--------|
| 1 | head |
| 2 | necklace |
| 3 | backpack |
| 4 | armor |
| 5 | right |
| 6 | left |
| 7 | legs |
| 8 | feet |
| 9 | ring |
| 10 | ammo |

`useItem` / `moveItem` com só `slot` 1–10 usam esta tabela. Offsets só no JSON do `buildId`.

Calibração inicial (char no mapa; usar os valores atuais exatos):

```
node tools/p1-calibrate.js --health 185 --maxHealth 185 --mana 50 --maxMana 50 --level 8 --exp 12345 --cap 400
node tools/p1-calibrate.js --refine --x 32097 --y 32191 --z 7 --inventory --id 2854 --slot 3
node tools/p1-calibrate.js --refine --minX 700 --maxX 900 --minY 1750 --maxY 2000 --z 7
node tools/p1-calibrate.js --refine --z 7 --battle
node tools/p1-calibrate.js --refine --z 7 --moved
node tools/p1-calibrate.js --refine --z 7 --moving
```

Perfil externo: `<pasta da OTCApi.dll>/calibrations/{buildId}.json`, para leitura e gravação durante a calibração.
Em ambas as arquiteturas: `dist/calibrations/`. Os artefatos finais ficam juntos
em `dist/`, com o sufixo `-Win32` ou `-x64`; injector, DLL e processo devem ter
a mesma arquitetura. A pasta do jogo não é consultada nem recebe JSON. Após promover o
perfil para `configs/calibrations/` e recompilar, a DLL usa a cópia embutida quando
o arquivo externo não existe ou é inválido. Calibrar tenta escrever nessa pasta;
usar uma DLL com perfil incorporado não depende dela.

O primeiro comando exige uma cadeia não vazia (`chainSteps>0`) para salvar; somente
um restart comprova que ela é restaurável entre processos. Quando X/Y exatos não forem conhecidos, os quatro limites `--minX/--maxX/--minY/--maxY` formam um retângulo inclusivo e devem ser usados junto com `--z`; candidatos fora dele são rejeitados. Os limites são apenas evidência do wizard e não são gravados no perfil. O refine com `--id/--slot` exige ver o client id naquele slot; `--battle` só deve ser usado com o estado/swords ativo. Depois, reinicie o mesmo exe e exija `ready` sem hints antes de distribuir o perfil.

## Container (P2)

Janelas **abertas** no cliente. Poll ~250 ms. O app mantém `index → container`.

| Evento | Quando |
|--------|--------|
| `containerOpen` | janela nova (já aberta no connect, ou você abriu agora) |
| `containerUpdate` | `name` / `capacity` / `items` daquele `index` mudaram |
| `containerClose` | janela fechou — some do mapa do app |

`data`: `index` (id da janela no cliente, 0–15), `name`, `capacity` (slots da grade), `items[{slot,id,count}]`. `slot` é 0-based. Só entram itens **existentes**; slot vazio não aparece. `id` é client id.

`getState what=containers` devolve o array das abertas agora.

Não entram: BP só **equipada** (slot 3) sem a grade aberta; conteúdo de uma BP **fechada** que está dentro de outra. Várias janelas ao mesmo tempo — cada uma com `index` próprio.

```
node tools/p2-calibrate.js --name Backpack --capacity 20
node tools/pipe_test.js --only containers
```

`--name` ignora maiúsculas; `--capacity` = slots da grade, **não** a cap do char.

A calibração inicial exige a janela aberta com pelo menos um item. Se a descoberta
habitual não encontrar uma cadeia para a coleção, o P2 pode buscar um mapa no
módulo entre todos os objetos com o nome informado. Essa alternativa exige árvore,
índices, capacidade e itens coerentes; uma referência antiga de uma bag fechada
não basta. O perfil só recebe a cadeia relativa ao módulo após validar a leitura.

## Creature (P3)

Criaturas que o cliente conhece **no mesmo andar**, até ~12 SQM, **sem o personagem local**. Poll ~250 ms. Sem parser de pacote.

O **app** mantém `id → creature`. A API não guarda histórico: emite diffs e um snapshot no `getState` / connect.

| Evento | Quando | `data` |
|--------|--------|--------|
| `creatureAppear` | nova (já no alcance no connect, ou acabou de entrar) | objeto completo |
| `creatureUpdate` | qualquer campo mudou | objeto completo |
| `creatureDisappear` | saiu do alcance / andar / lista | só `id`, `name` |

| Campo | Tipo | Significado |
|-------|------|-------------|
| `id` | uint32 | Creature id do **cliente**. P4 `attack` / `follow` usam como `creatureId`. Não é ponteiro. |
| `name` | string | Nome visível. |
| `x`, `y`, `z` | int | Tile. |
| `hpperc` | int | Vida 0–100. |
| `kind` | string | `player` / `monster` / `npc` / `unknown`. Faixa do `id`: `≥ 0x80000000` npc, `≥ 0x40000000` monster, `≥ 0x10000000` player. |
| `skull` | int | 0–6; 0 se o layout não tiver skull. |
| `inViewport` | bool | Mapa desenhado (~8 SQM leste/oeste, ~6 norte/sul). |
| `distance` | int | Chebyshev até o LocalPlayer. Diagonal = 1. `-1` se o tile do char não estiver disponível. |

Regras: sem LocalPlayer; mesmo `z`; corte 12 SQM (fora do ecrã mas ≤12 continua, `inViewport: false`); a janela de battle do cliente pode mostrar menos. Trocar de andar: disappear dos que ficaram + appear dos do novo `z`.

O app escolhe o alvo (`kind`, `inViewport`, `distance`, `hpperc`); a DLL só pede ao cliente atacar, seguir ou chase.

```
node tools/p3-calibrate.js --name Rat
node tools/pipe_test.js --only creatures
```

## Map (P5)

Tiles em volta do personagem, lidos da memória (não de pacote). Sem eventos contínuos: o app pede `getState what=map`. Offsets não saem na pipe.

```json
{"action":"getState","what":"map","requestId":"1"}
```

```json
{"type":"event","event":"state","data":{"what":"map","data":[{"x":32097,"y":32191,"z":8,"items":[{"id":100,"count":1}]}],"requestId":"1"}}
```

`data` é um array de `{x,y,z, items:[{id,count}]}`.

| Regra | |
|-------|--|
| Andar | só o mesmo `z` do `playerStats.position` |
| Raio | Chebyshev; valor no JSON do `buildId` (`map.radius`, default 8) |
| Vazio | tile sem items **não entra** |
| Player | o tile sob o char **entra** se tiver chão (`items[0]` = ground) |
| `items` | só Item (chão primeiro, depois a pilha). Sem criaturas (P3) e sem effect/missile. `id` é client id |

Não há `tileAppear` / `tileUpdate` neste recorte. No connect a DLL não despeja o mapa (o snapshot inicial continua player / inventory / containers / creatures).

Calibração (char no mapa, P1 posição, perfil com `actions.mapRva` do P4):

```
node tools/p5-calibrate.js
node tools/p5-calibrate.js --id 351
node tools/pipe_test.js --only map
```

`--id` é opcional: client id de um item **no tile do player** (chão ou drop). Sem `--id`, qualquer Item nesse tile serve.

Verificado ao vivo no KarmaOT_GL: `getState what=map` devolve o chão sob o player; andar 1 SQM e pedir de novo — a janela acompanha.

## Look

Quando o **cliente** faz look (Shift+click no OTClient, look na battle list, ou a ação `look` na pipe), a DLL lê o Thing que o jogo já escolheu. Sem pacote. O app recebe o **client id** do item (ou o id P3 da criatura) e as **coordenadas do SQM** se o look foi no mapa.

Não há poll: cada look emite um evento. O último fica em `getState what=look`.

```json
{"type":"event","event":"look","data":{"kind":"item","id":3031,"count":1,"x":32097,"y":32191,"z":7}}
```

```json
{"type":"event","event":"look","data":{"kind":"creature","id":1073793728,"name":"Rat","x":32097,"y":32190,"z":7}}
```

```json
{"type":"event","event":"look","data":{"kind":"item","id":2854,"count":1,"container":0,"slot":3}}
```

| Campo | Quando |
|-------|--------|
| `kind` | sempre: `item` ou `creature` |
| `id` | sempre. Item = client id (o mesmo de P1/P2/P5). Criatura = id P3 (`creatureId`) |
| `count` | item, se o layout tiver count |
| `x`,`y`,`z` | look **no mapa** (SQM clicado). Ausente em inventário/container |
| `container`,`slot` | look numa janela P2 (`slot` 0-based) ou no inventário (`slot` 1–10, sem `container`) |
| `name` | criatura, se o layout P3 tiver nome |

Look no chão de um SQM: `kind` `item`, `id` do item (chão ou topo da pilha, o que o cliente olhou), mais `x,y,z`. Look numa criatura em cima do tile: `kind` `creature` e as mesmas coordenadas.

A DLL precisa do RVA de `Game::look` (bloco `actions.lookRva`, resolvido como as outras ações P4). Sem isso não há evento — o primeiro `look` na pipe também dispara o scan.

### Ação `look`

O app pode pedir ao cliente para olhar. O jogo mostra o “You see…”. A pipe responde `action` **e** emite o evento `look`.

Origem (uma):

- `x`,`y`,`z` — tile (topo do SQM, o que o cliente olharia)
- `container` + `slot` — janela P2, slot **0-based**
- só `slot` 1–10 — inventário
- `creatureId` — criatura P3

```json
{"action":"look","x":32097,"y":32191,"z":7,"requestId":"1"}
{"action":"look","container":0,"slot":0,"requestId":"2"}
{"action":"look","slot":6,"requestId":"3"}
{"action":"look","creatureId":1073793728,"requestId":"4"}
```

Resposta `action`: `ok`, `via: "client"`, mais os mesmos campos do evento (`kind`,`id`, coordenadas ou container/slot).

```json
{"action":"getState","what":"look","requestId":"5"}
```

`data` é o último look ou `{}`.

```
node tools/pipe_test.js --only look
node tools/p4-test.js --look 32097 32191 8
node tools/p4-test.js --lookItem 0 0
node tools/p4-test.js --lookInv 6
node tools/p4-test.js --lookCreature 1073793728
```

Verificado no KarmaOT_GL (`pipe_test --only look`, Shift+click): item em janela (`id` 3067, `container` 0, `slot` 0) e SQM (`id` 409, `x` 32346, `y` 32224, `z` 7).

## Ações (P4)

A DLL pede ao **cliente** para agir. Sem pacote XTEA.

Funções do OTClient correm na **thread da janela** (`PostMessage`). Sem fallback de tecla nessas ações: ou o cliente obedece, ou `ok` é false. `step` / `stop` são tecla (`via: "keys"`).

O modelo de ponteiro também faz parte do ABI calibrado. Builds `shared_ptr` usam T* + control block; builds raw/intrusivos podem ter outra pilha e callbacks dispatcher-backed. A DLL não converte um layout no outro. No Win32, `attack` raw tenta enfileirar a chamada em `EventDispatcher::addEvent`, identificado nos metadados do cliente. O alvo é resolvido novamente dentro do callback; a resposta aguarda sua execução por até 5 segundos. Um callback ainda na fila ao expirar é cancelado, sem executar ataque atrasado. Sem uma rota compatível, retorna `attack dispatcher failed`. `follow` raw continua bloqueado; cancel com ponteiro vazio é uma operação separada. Esta rota nova requer validação ao vivo por build.

Resposta: evento `action`.

```json
{"type":"event","event":"action","data":{"ok":true,"action":"say","via":"client","requestId":"1"}}
```

| Campo | Quando |
|-------|--------|
| `ok` | sempre |
| `action` | eco do comando |
| `via` | `client` ou `keys` (se a ação chegou a tentar) |
| `error` | se `ok` é false |
| `follow` | só em `attack` se o comando trouxe `follow` |
| `requestId` | se o comando trouxe |

`attack` / `follow` **não** são toggle: repetir o mesmo `creatureId` devolve `ok: true` e **não** cancela. Cancelar é `cancelAttack` / `cancelFollow`.

`Game::follow` **cancela** o ataque no cliente. Atacar e perseguir o alvo é chase: `attack` com `"follow":true` (`Game::setChaseMode`), não o comando `follow`.

### Tabela

| `action` | Payload | Como |
|----------|---------|------|
| `say` | `text`, `mode?` (`say`/`whisper`/`yell` ou `1`/`2`/`3`) | `Game::talkChannel`. Máx. 255 chars. Sem caixinha. |
| `step` | `direction` | tecla, ~80 ms. `north`/`south`/`east`/`west` (`n`/`s`/`e`/`w`, `up`/`down`/`left`/`right`). Diagonais `ne`/`nw`/`se`/`sw`. Ou `0`–`7` (N E S W NE SE SW NW). |
| `stop` | | solta setas e numpad de movimento |
| `logout` | | enfileira `Game::safeLogout` na thread do cliente, inclusive com a janela minimizada/oculta (fallback por `EventDispatcher::addEvent`), sem diálogo e sem simular tecla. O cliente/servidor ainda decide se o personagem pode sair (por exemplo, battle). |
| `walkTo` | `x,y,z` | `LocalPlayer::autoWalk`. Sem caminho: `ok: false`, `error: "no path"`, `via: "client"`. |
| `click` | `x,y,z` | `Game::use` no `getTopUseThing` daquele SQM (escada de mão, alavanca, porta). Não é mouse nem `walkTo` |
| `setLight` | `enabled` bool; `intensity?` número `0..1` | Ajusta localmente a luz ambiente mínima do mapa. Ligado sem intensidade usa `1.0`. Desligado restaura o valor anterior. Não envia ação ao servidor. |
| `attack` | `creatureId` (id P3). `follow?` bool | `Game::attack`. `follow: true` liga chase e ataca; `false` desliga chase e ataca; omitido não mexe no chase. Alvo sumiu: `ok: false`. |
| `cancelAttack` | | `Game::attack` com `CreaturePtr` vazio |
| `follow` | `creatureId` | `Game::follow` — **só** seguir, sem atacar. Já seguir esse alvo não cancela. |
| `cancelFollow` | | `Game::follow` vazio |
| `useItem` | ver abaixo | sem `to`: `Game::use`. com `to`: `Game::useWith` |
| `moveItem` | ver abaixo | `Game::move` |
| `look` | `x,y,z` **ou** `container`+`slot` **ou** `slot` 1–10 **ou** `creatureId` | `Game::look`. Emite evento `look`. Ver secção Look |
| `getState` | `what` | P1–P3, P5 e `look` (evento `state`, não `action`) |
| `ping` | | evento `pong` |

### `setLight`

Ajusta o brilho mínimo de toda a área visível do mapa, apenas neste cliente.
`intensity: 1` elimina a escuridão; valores intermediários mantêm parte das sombras.
Não consome tocha, mana ou item, nem altera a luz recebida pelos outros jogadores.
O primeiro `enabled:true` guarda o brilho anterior; `enabled:false` o restaura.
O efeito dura enquanto o mesmo mapa visual existir e não for alterado pelas opções do cliente.

```json
{"action":"setLight","enabled":true,"intensity":1,"requestId":"light-on"}
{"action":"setLight","enabled":false,"requestId":"light-off"}
```

Teste: `node tools/p4-test.js --light 1` e `node tools/p4-test.js --lightOff`.
Suporte inicial: accessors MSVC Win32 reconhecidos; outra assinatura retorna
`light not resolved` sem alterar a memória. A resposta de sucesso inclui a intensidade aplicada.
Validado no Valantis `pe-61beeb3a-…`: intensidade `1` clareou a caverna e desligar
restaurou `0`. A regressão ao vivo no KarmaOT_GL permanece pendente.
Validado também no TibiaScape x64 `pe-55ddf475-…`: dois ciclos consecutivos de
`--light 1` / `--lightOff` clarearam e restauraram `0`, com `ok:true` e
`via:"client"`. Os RVAs e o ABI permanecem específicos do perfil dessa build.

### `useItem`

Origem (uma):

- `container` + `slot` — janela P2, slot **0-based**
- só `slot` 1–10 — inventário
- ou `from: {container?, slot, id?}`

`id?` — se vier e o slot tiver outro client id: `ok: false`, `error: "item not found"`.

`to` omitido = Use (comida, abrir BP). `to` presente = Use with:

| `to` | Alvo |
|------|------|
| `{"self":true}` ou `true` | o próprio char |
| `{"creatureId":…}` | criatura P3 |
| `{"container":0,"slot":1}` | item em container |
| `{"slot":6}` | item no inventário |
| `{"x":…,"y":…,"z":…}` | tile (`getTopUseThing` — pá no buraco, corda no poço) |

### `click`

`x,y,z` obrigatórios. O cliente usa o Thing de uso naquele SQM (`getTopUseThing` + `Game::use`). Escada de mão, alavanca, porta, buraco. Sem clique de rato.

Escada em que o char **anda** para mudar de andar: `walkTo` no mesmo tile. `click` **não** anda — o char tem de estar ao alcance de Use (em geral 1 SQM).

```json
{"action":"click","x":32346,"y":32224,"z":7,"requestId":"20"}
{"action":"setLight","enabled":true,"intensity":1.0,"requestId":"21"}
{"action":"setLight","enabled":false,"requestId":"22"}
```

Verificado no KarmaOT_GL: `click` 32339,32226,8 → `"ok": true`, `"via": "client"`, o char usa o SQM.

### `moveItem`

Mesma origem que `useItem`. `to` **obrigatório**. `count?` omitido = pilha inteira.

| `to` | Destino |
|------|---------|
| `{"container":0,"slot":1}` | slot de janela (vazio ou ocupado) |
| `{"slot":10}` | slot de inventário |
| `{"x":…,"y":…,"z":…}` | chão |

Não há `move` para criatura.

### Exemplos

```json
{"action":"say","text":"exura","requestId":"1"}
{"action":"say","text":"exevo gran mas vis","mode":"yell","requestId":"1b"}
{"action":"step","direction":"north","requestId":"2"}
{"action":"stop","requestId":"3"}
{"action":"logout","requestId":"3b"}
{"action":"walkTo","x":32097,"y":32191,"z":8,"requestId":"4"}
{"action":"useItem","container":0,"slot":0,"id":2920,"requestId":"5"}
{"action":"useItem","slot":3,"requestId":"6"}
{"action":"useItem","slot":6,"to":{"self":true},"requestId":"7"}
{"action":"useItem","container":0,"slot":1,"to":{"creatureId":1073793728},"requestId":"8"}
{"action":"useItem","container":0,"slot":1,"to":{"x":32097,"y":32191,"z":7},"requestId":"9"}
{"action":"attack","creatureId":1073793728,"requestId":"10"}
{"action":"attack","creatureId":1073793728,"follow":true,"requestId":"10b"}
{"action":"attack","creatureId":1073793728,"follow":false,"requestId":"10c"}
{"action":"cancelAttack","requestId":"11"}
{"action":"moveItem","container":0,"slot":0,"to":{"container":0,"slot":1},"requestId":"12"}
{"action":"moveItem","container":0,"slot":0,"to":{"x":32097,"y":32191,"z":7},"count":1,"requestId":"13"}
{"action":"moveItem","slot":10,"to":{"container":0,"slot":0},"requestId":"13b"}
{"action":"follow","creatureId":1073793728,"requestId":"14"}
{"action":"cancelFollow","requestId":"15"}
{"action":"look","x":32097,"y":32191,"z":7,"requestId":"16"}
{"action":"look","container":0,"slot":0,"requestId":"17"}
{"action":"look","slot":6,"requestId":"18"}
{"action":"look","creatureId":1073793728,"requestId":"19"}
{"action":"click","x":32346,"y":32224,"z":7,"requestId":"20"}
```

### Erros (`action.error`)

| `error` | Típico |
|---------|--------|
| `empty text` | `say` sem texto |
| `talk not resolved` / `talk call failed` | `say` |
| `bad direction` | `step` |
| `no game window` | `step` / `logout` |
| `logout not resolved` / `logout call failed` | `Game::safeLogout` não foi identificado ou a chamada do cliente falhou |
| `missing coordinates` / `bad coordinates` | `walkTo` |
| `no path` | `walkTo` — cliente não achou caminho (`via` continua `client`) |
| `autoWalk not resolved` / `autoWalk call failed` / `no local player` | `walkTo` |
| `missing slot` / `bad slot` / `item not found` | `useItem` / `moveItem` |
| `use not resolved` / `use call failed` | Use simples |
| `useWith not resolved` / `useWith call failed` / `tile not resolved` / `bad target` / `target item not found` / `creature not found` | Use with |
| `missing dest` | `moveItem` sem `to` útil |
| `move not resolved` / `move call failed` | `moveItem` |
| `missing creatureId` / `creature not found` | `attack` / `follow` |
| `chase not resolved` | `attack` com `follow` bool e `setChaseMode` não achado |
| `attack not resolved` / `attack call failed` | `attack` / `cancelAttack` |
| `attack dispatcher failed` | `attack` raw: dispatcher incompatível, falha de execução ou timeout |
| `follow not resolved` / `follow call failed` | `follow` / `cancelFollow` |
| `cancelAttack call failed` / `cancelFollow call failed` | cancel |
| `missing look target` / `look not resolved` / `look call failed` / `tile not resolved` | `look` |
| `missing coordinates` / `bad coordinates` / `use not resolved` / `tile not resolved` / `use call failed` | `click` |
| `missing enabled` / `bad intensity` / `light not resolved` / `light call failed` / `light not active` | `setLight` |

`not implemented` só se a ação existir no contrato e ainda não no C++. Nas ações da tabela isso não deve aparecer.

### Teste

Cliente injetado, personagem no mapa. `creatureId` = `id` de um `creatureAppear`.

```
node tools/p4-test.js --say exura
node tools/p4-test.js --step north
node tools/p4-test.js --logout
node tools/p4-test.js --walkTo 32097 32191 8
node tools/p4-test.js --useItem 0 0
node tools/p4-test.js --useInv 3
node tools/p4-test.js --useInv 6 --toSelf
node tools/p4-test.js --useItem 0 1 --toCreature 1073793728
node tools/p4-test.js --useItem 0 1 --to 32097 32191 7
node tools/p4-test.js --attack 1073793728
node tools/p4-test.js --attack 1073793728 --follow
node tools/p4-test.js --attack 1073793728 --noFollow
node tools/p4-test.js --cancelAttack
node tools/p4-test.js --moveItem 0 0 --toItem 0 1
node tools/p4-test.js --moveItem 0 0 --toInv 10
node tools/p4-test.js --moveItem 0 0 --to 32097 32191 7 --count 1
node tools/p4-test.js --follow 1073793728
node tools/p4-test.js --cancelFollow
node tools/p4-test.js --look 32097 32191 8
node tools/p4-test.js --lookItem 0 0
node tools/p4-test.js --lookInv 6
node tools/p4-test.js --lookCreature 1073793728
node tools/p4-test.js --click 32346 32224 7
```

`--useItem` / `--moveItem` = índice da janela + slot 0-based. `--useInv` / `--moveInv` = inventário 1–10. `--follow` sozinho = ação seguir; no `--attack` = chase. `--look` = SQM; `--lookItem` / `--lookInv` / `--lookCreature` = item ou criatura. `--click x y z` = Use no SQM.

`say`, `logout`, `walkTo`, `useItem`, `attack`, `cancelAttack`, `follow`, `cancelFollow`, `moveItem`, `look`, `click` devem trazer `"via":"client"`. `step` / `stop` → `"via":"keys"`.

Validado ao vivo no Valantis `pe-61beeb3a-…`: `logout` retornou `ok:true`,
`via:"client"` e o personagem saiu do jogo por `Game::safeLogout`, inclusive com
o cliente minimizado e sem HWND enumerável, pela rota `EventDispatcher::addEvent`.

Verificado no KarmaOT_GL: as ações da tabela (incluindo `attack` com/sem chase, Use with no tile e `click` no SQM). O char fala, anda, usa item, ataca (com ou sem chase), segue, move item e usa escada/SQM. `look` no Shift+click emite `id` e `x,y,z` (ou `container`/`slot`).

## Calibração (wizard neste repo)

Comandos `calibration` (`hints`, `scan`, `refine`, `scanContainers`, `scanCreatures`, `scanMap`, `save`) e `getState what=calibration` são do `tools/`, não da API de script do app. Não misturar offsets no JSON da pipe de jogo.

Os hints P1 de posição aceitam uma coordenada exata (`x`, `y`, `z`) ou um retângulo inclusivo (`minX`, `maxX`, `minY`, `maxY`, `z`). O retângulo só é válido com os quatro limites e `z`, e não altera o comportamento dos scans que não o informam.

Coordenadas de mapa X/Y não usam cortes de região (como o antigo mínimo 1000). Leituras e ações aceitam toda a faixa representável pelo cliente, `0..65535`; `z` permanece em `0..15`. Limites `minX/maxX/minY/maxY` só restringem a calibração quando enviados explicitamente como hints.

Na calibração P3, `creatureName` pode ser acompanhado de `z` (0–15). Nesse modo o andar informado é a evidência de proximidade e o scan não reutiliza X/Y não comprovados do P1; o uso normal continua preferindo a posição P1 completa quando ela está válida.

`--z` não usa a origem `(0,0)` como centro de busca: X/Y desconhecidos são distintos de coordenadas zero válidas. O filtro por andar também vale ao reutilizar um layout P3 existente; sem `--z`, a proximidade continua baseada na posição completa do player.

O evento/estado de calibração acrescenta `capacityScale`, `chainSteps`, `hasPosition`, `hasStates`, `hasInventory`, `hasContainers`, `hasCreatures`, `hasTalk` e `hasMap`. São diagnósticos do wizard, não capability negotiation do app.

Fluxo obrigatório para exe novo:

1. P1 com todos os hints exatos e `chainSteps>0`.
2. Refine de posição/states/inventário com evidência observável.
3. P2 com janela aberta, P3 com criatura visível.
4. Ações seguras P4; um `look x y z` pode resolver as âncoras necessárias ao P5.
5. P5, restart sem scan, promoção do JSON e teste exclusivo no Monitor.

Comandos, critérios de aceite, precedência do perfil, diagnóstico de `HP copy ignored` e estudo da build DeusOT atual: [`configs/calibrations/README.md`](../configs/calibrations/README.md#fluxo-completo-para-um-exe-novo).

