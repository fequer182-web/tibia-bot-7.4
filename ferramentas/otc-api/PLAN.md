# Plano (só API)

**Etapa 1 fechada (P0–P5).** Baseline verde no KarmaOT_GL: inject → pipe, self, containers, battle, ações, mapa.

Armazenamento de calibração: o catálogo `configs/calibrations/` é compilado na DLL. O JSON ao lado da DLL permanece como override gravável durante a calibração; ausente ou inválido, o perfil embutido do `buildId` é usado. O comando `tools/embed-calibration.ps1` promove o resultado e recompila a DLL portátil.

Daqui pra frente: **features** — campo novo na pipe ou ação que o jogo obedece. Sem P6 automático. Sem inventar backlog. Uma feature por vez; o contrato vive em `docs/API.md`.

Implementar **neste repo, do zero**. Não copiar `new-poc` / POC2 / henrique-poc.

App, scripts, healer, cavebot: outro repositório.

## Features (depois da etapa 1)

Cada feature: contrato em `docs/API.md` → código → verificação ao vivo.

Em 08/09, fluxo simplificado de coordenadas: evento `monsterEnd` para monstros
visíveis, sem `watch`, alvo de ataque, mapa ou abertura. HP 0 é marcado
`deathObserved:true`; desaparecimento informa última posição e não confirma morte.
Teste: `node tools/pipe_test.js --only deaths`. Contrato em `docs/API.md`.
Validado ao vivo em 08/09 no Valantis
`pe-165c7c71-6a9f25d8-00b48000-006ce200-014c`: Sheep 1073765702,
HP 0, posição 32060,32210,7, `deathObserved:true`, `positionSource:current`,
`reason:zero-hp`. Evidência: evento da pipe enviado pelo usuário após matar o
monstro. Regressão Karma permanece pendente; essa validação não cobre abertura
de corpos nem transfere validações entre builds.
Build Win32 Release concluído; CTest 4/4 e testes Node do consumidor passaram.
Diagnóstico de 08/09 no PID 21716: coleção P3 tinha 6 objetos; as Sheep
1073765704 e 1073765705 permaneciam nela com HP 0 e posição 65535,65535,255.
Outros objetos removidos tinham HP 100. O leitor fresco rejeitava a coleção
inteira nesses casos. Corrigido somente esse caminho: exclui sentinelas
validadas, preserva IDs com HP 0 e emite a última posição válida acompanhada,
com `positionSource:last-seen`. Não altera o leitor/scan P3 estabelecido.
O reteste após a correção confirmou `positionSource:current`; a variante de
morte com `positionSource:last-seen` tem cobertura automatizada, ainda sem
evento de morte confirmado ao vivo. O snapshot inicial `disappeared` com
HP 100 era histórico, não outra morte nem o estado atual da criatura.

Corpo do alvo — implementação de `watchCorpse`, evento/estado `corpse` e
`openCorpse` (aproximação, depois use com ID revalidado). A correlação identifica
automaticamente uma única inserção estável no tile após perder o alvo; IDs
informados pelo consumidor são filtro opcional. Não comprova autoria do kill
nem tipo semântico do item (um drop coincidente pode parecer corpo). Abertura
raw de 4 bytes falha fechada; não há nova calibração ou RVA presumido.
Validação live do ciclo e regressão Karma pendentes. Contrato: `docs/API.md`.
Em 05/09, build Win32 Release de DLL/injector concluído e CTest 3/3 passou
(correlação de corpo, profile store e seed de calibrações). Valantis foi fechado
com autorização para liberar a DLL; a nova versão ainda não foi injetada.
Atualização do mesmo dia: `watch` sem ID passou no teste do consumidor da pipe;
detecção automática com dois polls estáveis passou no teste C++, incluindo
rejeição de substituição de chão, pilhas, múltiplas inserções e item transitório.
DLL/injector recompilados em Release; validação live continua pendente.
Após relato de Sheep `watching -> expired`, corrigida a mistura de mapa fresco
com cache P3 antigo: acompanhamento lê mapa antes de uma leitura P3 sem cache,
validando coleção vazia separadamente de falha de leitura. A rota P3 existente
permanece intacta. Expiração agora expõe motivo e itens comparados. Build Release,
CTest 3/3 e teste Node passaram; reteste Sheep e regressão Karma pendentes.

- **Pronto:** o campo aparece na pipe **ou** o cliente obedece a ação. Sem pacote XTEA. Offsets só no `{buildId}.json`.

Refino já na baseline: `playerStats.position` com o char no mapa (offset P1; se falhar, tile do LocalPlayer / layout P3, offset vivo em cache).

Look — **Pronto** (KarmaOT_GL, Shift+click): evento `look` com item em janela (`id` 3067, `container`/`slot`) e SQM (`id` 409, `x,y,z` 32346,32224,7). Sem pacote. Contrato: `docs/API.md`.

Click — **Pronto** (KarmaOT_GL): `click` 32339,32226,8 → `"via":"client"`, o char usa o SQM (escada). Contrato: `docs/API.md`.

Iluminação local — **validada no Valantis** `pe-61beeb3a-…` em 03/09:
`setLight enabled=true intensity=1` clareou a caverna (confirmação visual do usuário);
`enabled=false` restaurou o brilho anterior `0`, com leitura prévia `1` no getter.
Métodos de UIMap descobertos pelo bind e ABI dos accessors validado; RVAs salvos
somente no perfil local da DLL. Intensidade ajustável `0..1`, sem item ou pacote.
Build Win32 e CTest passaram. Regressão Karma live permanece indisponível.

## Compatibilidade por build — estado atual

“P0–P5 fechado” continua significando a baseline ao vivo do KarmaOT_GL; não implica que todo exe novo herda todas as ações antes de calibrar e validar o próprio ABI.

Em 07/09, o caminho de calibração recebeu suporte aditivo a clientes x64:
validação do control block/RTTI MSVC x64, descoberta de valores `shared_ptr` de
16 bytes nos candidatos de containers e vetores de criaturas, e campo
`architecture` no `dllStatus`. O inventário já tentava larguras de um e dois
ponteiros nativos. Os candidatos Win32 alterados mantêm a ordem histórica
perfil → 8 → 4 bytes.
Build de DLL/injector e CTest 3/3 passaram em Win32 e x64. Após autorização
explícita, o TibiaScape PID 13080 foi encerrado para liberar a DLL antiga; o link
x64 terminou em `dist/` como `OTCApi-x64.dll` e `OTCInjector-x64.exe`; o build interno permanece em `build-x64/`. O cliente precisa ser reaberto e reinjetado.
No novo processo, o build `pe-55ddf475-6a82c258-00fdb000-009d6200-8664`
emitiu P1 (stats, posição e inventário), mas a cadeia atravessa uma alocação de
FrameBuffer e ainda não passou restart. O P2 encontrou uma Bag antiga retida por
Lua antes da Bag aberta. Foi adicionado um fallback conservador, executado somente
após a descoberta existente falhar, que valida o `std::map` completo ancorado no
módulo e todos os candidatos de mesmo nome. CTest 4/4 passou em Win32 e x64;
validação P2 ao vivo, P3–P5, restart sem hints e regressão Karma seguem pendentes.
No restart seguinte, a cadeia P1 de quatro passos falhou porque o primeiro objeto
continuou sendo `FrameBuffer`, mas seu membro +1680 passou a conter `1`. A coleção
P2 restaurada revelou o `LocalPlayer` em um campo próximo no módulo e validou o
layout completo (94/155 HP, 5/5 mana, nível 2, exp 117, cap 329, posição e
inventário). A recuperação agora aceita stride de inventário x64 de 16 bytes e,
na ausência de metadata Game, usa uma busca limitada ao entorno da coleção P2,
com duas observações antes de reparar somente a cadeia P1. Builds e CTest 4/4
passaram. No PID 10092, o restart/reinject restaurou a coleção P2 vazia, confirmou
o mesmo LocalPlayer duas vezes e salvou uma cadeia direta de dois passos. A pipe
voltou a emitir `playerStats` com HP 94/155, mana 5/5, nível 2, exp 117, cap 329 e
posição 32095,32204,7; restauração P1 desta build está validada ao vivo.

| Build | Leitura P1/P2/P3/P5 | Ações | Estado de regressão |
|-------|----------------------|-------|---------------------|
| KarmaOT_GL `pe-407f0b0e-…` | verde | P4 completo, look e click verdes | baseline histórica ao vivo |
| DeusOT `pe-a6b7516c-6a8eddb3-01ba3000-0121a200-014c` | stats, posição/states, inventário, containers, criaturas e mapa validados | ações seguras têm perfil; `attack`/`follow` raw não nulos ficam desativados/fail-closed | leitura pronta para Monitor; regressão Karma live indisponível nesta sessão |
| DeusOT `pe-a9376da6-6a885df0-01b9c000-01215000-014c` | somente fechamento P1 histórico | não calibrado | outro PE; não reutilizar no build atual |
| Valantis `pe-61beeb3a-6a9365ea-00b47000-006cdc00-014c` | P1 recuperado e restaurado diretamente após restart; P2/P3/P5 calibrados no log atual | `attack` raw executado via `EventDispatcher::addEvent`; `logout` via `Game::safeLogout` validado com janela aberta e minimizada/sem HWND; `follow` raw não validado | falta restart completo de P2–P5, ciclos de eventos, movimento de mapa e regressão Karma |
| TibiaScape x64 `pe-55ddf475-6a82c258-00fdb000-009d6200-8664` | P1/P2/P3/P5 restaurados após restart sem hints; mapa acompanhou a posição atual | `say`, `walkTo`, tile `look` e `setLight` on/off validados ao vivo via cliente | ciclos P2/P3, demais ações P4 e regressão Karma pendentes; perfil promovido |

P4 `say` inicialmente falhou com `talk not resolved`: a build x64 referencia o
nome do bind, o Game inline e o método por `LEA [RIP+disp32]`, enquanto o scanner
histórico reconhecia imediatos x86. Foi acrescentado um decoder x64 limitado como
fallback; o caminho Win32 continua compilado sem essa lógica. DLL/injector e CTest
4/4 passaram nas duas arquiteturas. No primeiro reteste, a pipe expirou antes da
descoberta terminar; nenhuma função
foi chamada e nenhum RVA foi salvo. O processo permaneceu responsivo. O decoder
estava consultando páginas byte a byte sobre toda a seção executável; a busca foi
substituída por uma passagem direta na seção previamente validada e recompilada.
Após restart/reinject, `say calibration-test` retornou `ok:true`, `via:"client"` e
o efeito foi confirmado ao vivo. O perfil da build persistiu `gameRva=15717232` e
`talkChannelRva=1344928`. Isso fecha somente `say`; as demais ações P4 continuam
sujeitas à descoberta e validação individual de ABI.
O primeiro `walkTo` falhou fechado com `autoWalk not resolved` e manteve
`autoWalkRva=0`: `pickAutoWalk` ainda procurava somente imediatos x86, apesar de
`say` já ter o fallback RIP-relative. A busca de string e os alvos próximos agora
usam o mesmo decoder somente em `_M_X64`; Win32 permanece inalterado. Builds e
CTest 4/4 passaram em x86/x64. Após reinjeção, `walkTo 32096 32204 7` moveu o
personagem e persistiu `autoWalkRva=1453952`.
O `look` seguinte falhou fechado com `look not resolved` e manteve `lookRva=0`.
O helper `collectBinds` ainda procurava somente xrefs/imediatos x86; ele agora usa
o decoder RIP-relative somente em `_M_X64`. Builds e CTest 4/4 passaram novamente
nas duas arquiteturas. O reteste persistiu `lookRva`, `mapRva`, `getTileRva` e
`topUseRva`, mas falhou fechado com `tile not resolved`. O log mostrou um slot
x64 válido, com `Tile*` em `+0` e controle do `shared_ptr` em `+8`; a leitura ainda
usava `+4`. O RTTI local também tratava o TypeDescriptor como ponteiro absoluto.
Agora o slot usa `sizeof(void*)` e o RTTI MSVC x64 converte o RVA de 32 bits, sem
alterar o Win32. Builds e CTest 4/4 passaram. Após reinjeção, o tile `look` foi
executado e confirmado no cliente. O perfil manteve `lookRva=1332736`,
`mapRva=15718208`, `getTileRva=2230048` e `topUseRva=2606592`. RVAs de
`use`/`useWith`/`move` também foram descobertos, mas ainda não são verdes sem
validação individual do efeito ao vivo.
O primeiro P5 recebeu `--id 268436904` (fora da faixa de client item ids) e
falhou antes do filtro com `no Tile object at the player position`. O P4 havia
resolvido o mesmo Tile; o leitor de mapa tinha outra cópia do parser RTTI ainda
x86-only. `map_reader.cpp` agora aplica a conversão do TypeDescriptor RVA somente
em `_WIN64`. Builds e CTest 4/4 passaram em ambas as arquiteturas; repetir P5 sem
item hint primeiro.
Uma repetição sem `--id` ainda reutilizou o hint anterior porque o script não
enviava `hints` nesse caso. Agora ele sempre envia `mapItemId`, usando zero para
limpar, e a DLL trata zero como ausência de filtro. O script também recusa ids
fora de 100–59999 antes de conectar. Builds e CTest 4/4 passaram em x86/x64.
O reteste sem `--id` encontrou uma janela coerente de tiles no mesmo andar do
player em 32096,32204,7, marcou `hasMap:true` e salvou o layout `map-blocks` no
perfil desta build. Uma consulta posterior, sem recalibrar, leu o player em
32092,32203,7 e retornou exatamente a janela de raio 8 correspondente
(`x=32084..32100`, `y=32195..32211`), fechando o gate de movimento.
Após encerrar somente o PID que carregava a DLL, reabrir o mesmo PE e reinjetar,
a pipe entrou diretamente em `phase:"ready"`: player, inventário, Bag e criaturas
foram restaurados. Sem executar calibrador, `getState map` retornou a janela de
raio 8 em torno da nova posição 32097,32204,7. O JSON DLL-local foi promovido
para `configs/calibrations/` com SHA-256 idêntico. As ações ainda não confirmadas
visualmente continuam registradas como pendentes, não como verdes.
O P4 light exigiu três extensões x64 conservadoras em `actions.cpp`: descoberta
RIP-relative dos binds, reconhecimento dos pequenos accessors MSVC x64 com offsets
de UIMap/MapView iguais, e RTTI no qual TypeDescriptor é RVA de 32 bits mas
`vftable[-1]` contém um ponteiro nativo de 8 bytes para o COL. Após uma primeira
descoberta correta, a busca global de heap tornou-se intermitente quando a UI foi
recriada; no x64 ela agora prioriza uma janela limitada ao redor do LocalPlayer e
continua exigindo RTTI `UIMap` e `MapView`. Dois ciclos consecutivos ligaram em 1
e restauraram 0 com `ok:true`, `via:"client"`. O perfil promovido contém
`ambientLightRva=1504544` e `ambientLightGetRva=1505216`.
O primeiro P3 do TibiaScape foi salvo sem hint de HP e selecionou um campo enum de
4 bytes: Pigs visualmente em 100% apareciam como 0–3%. O utilitário agora sempre
aceita HP exato por `--hpperc 0..100`, sem presumir 100 quando omitido, para
uma barra diferente. O perfil da build precisa ser recalibrado com um Pig em 100%
antes de validar `attack`; a falha `creature not found` ocorreu depois que o ID
escolhido já havia saído da coleção e não prova a chamada corrigida.
O novo P3 salvou `hppercOffset=104`, tamanho 1, e passou a emitir 100% para os
Pigs. A leitura de coleções shared_ptr agora omite control blocks expirados antes
do snapshot. O `attack` x64 passa uma cópia temporária de 16 bytes por endereço,
como exige o ABI MSVC, e equilibra sua referência; `attack 1074106917` retornou
`ok:true`, `via:"client"`. IDs na faixa padrão 0x40000000..0x7fffffff agora
prevalecem como `monster` sobre campos auxiliares inconsistentes.
No restart seguinte, o candidato P3 `positionOffset=26/packed16` revelou-se
instável: um NPC foi lido em `65535,0,0` e essa mesma leitura contaminou a âncora
do filtro, ocultando os monstros. A leitura agora rejeita a coordenada sentinela
`0xffff`, ancora visibilidade na posição P1 independente e exige que uma nova
calibração de posição Creature reproduza o P1 no LocalPlayer quando disponível.
O perfil DLL-local voltou ao layout anteriormente observado
`positionOffset=32/int32`, mantendo HP em `104/1`; falta validar após restart.
Esse restart restaurou seis Pigs com HP, tipo e posição corretos. Depois de atacar
e cancelar, o ID `1074231224` desapareceu da coleção e o snapshot passou de seis
para cinco Pigs; portanto o segundo `creature not found` recusou corretamente um
ID encerrado, não uma mutação silenciosa. O utilitário P4 ganhou `--attackAt x y z`,
que consulta o snapshot atual e só ataca quando há exatamente uma criatura no
tile, sem escolher outro Pig pelo nome.
Apesar do retorno sem exceção, a validação visual mostrou que o cliente não iniciou
ataque nem perseguição. A revisão encontrou o contador forte do `shared_ptr` sendo
incrementado em `_Rep+4` também no x64; no ABI MSVC x64 ele fica depois do vptr de
8 bytes. O incremento agora usa `_Rep+sizeof(void*)`, preservando o Win32. Ataque
e cancelamento x64 também só retornam sucesso quando `m_attackingCreature` muda
para o alvo ou para nulo, respectivamente. Falta repetir a validação ao vivo.
A pós-condição passou, mas o efeito ficou somente visual e não houve perseguição
ou dano. A desassemblagem do PE no `attackRva=1342976` mostrou `movzx esi,r8b` e
o wrapper interno carregando `r8b=1`. O reteste mostrou que esse valor cancela
somente o estado visual e o servidor continua atacando; no corpo da função,
`test sil,sil; jne return` confirma que o booleano suprime o envio de protocolo.
O alvo vazio já representa cancelamento, portanto ataque e cancelamento via API
agora passam `false`; `follow` continua com seu ABI próprio de dois argumentos.
O reteste confirmou ao vivo perseguição/ataque com efeito no servidor e
`cancelAttack` interrompendo o ataque, além da remoção visual. Essas duas ações
estão verdes para este PE x64; `follow` como ação independente continua pendente.
Na revisão visual do inventário, o perfil começava em `arrayOffset=-368`, fazendo
colar sair como slot 1 e cabeça reaparecer como slot 10. Como o stride x64 é 16,
o perfil deste PE foi corrigido para `-384`. A ordem contratual permanece a enum
OTClient: cabeça, colar, backpack, armadura, mãos, pernas, botas, anel e munição.
Falta validar o snapshot corrigido após restart.

Atualização de 02/09: os perfis locais foram removidos com backup a pedido do usuário;
o catálogo fonte está sem JSON. As linhas Karma/DeusOT acima são evidência histórica,
não uma promessa de perfis distribuídos neste checkout. O JSON Valantis permanece
local na Release até validação final e promoção. Registro detalhado:
[calibração e portabilidade](docs/CALIBRATION-NOTES-2026-09-02.md).

Alterações de 02/09: perfis somente junto da DLL; build preservava JSON existente.
Desde 08/09, perfis aprovados também são incorporados no build e usados como
fallback sem arquivo externo;
recuperação P1 por Game com validação conservadora e save da cadeia; correção do
deref final P2; remoção de cortes regionais X/Y e sentinela distinta para `--z` P3.
O teste P1 de restart passou em processo diferente sem scan, mas não fecha as
pendências de outras superfícies ou clientes.

Descobertas portáveis da build DeusOT atual:

- LocalPlayer restaurável por chain image-relative a partir de `Game`; stats misturam `double` com experiência `uint64`.
- Largura de smart pointer não é igual à arquitetura: Karma usa `shared_ptr` de 8 bytes; este Deus usa ponteiros crus/intrusivos de 4 bytes em Item/Creature/Tile. A largura fica no perfil.
- P2/P3 continuam coleções calibradas, sem `if (DeusOT)`; P5 mantém blocos 32×32, mas o offset do vector de things também é dado do perfil.
- `Game::attack(CreaturePtr,bool)` raw foi identificado, porém alvo não nulo chamado diretamente pela WndProc reentrou callbacks do dispatcher e travou. Até existir rota dispatcher-backed validada, não preencher `attackRva`/`followRva` e retornar falha segura.

Runbook completo e critérios de aceite: `configs/calibrations/README.md`. A lacuna de regressão Karma é explícita: v1.3 não está mais disponível e o v1.4 local encerrou antes da injeção. Build verde e leitura de perfis antigos são verificações estáticas, não substitutos para esse live check.

## P0 — Esqueleto — verde

DLL injeta, pipe `\\.\pipe\OTCApi` sobe. `ready` **não** espera rede.

- CMake Win32 + x64 (final artifacts `OTCApi-{Win32,x64}.dll` + matching injector in `dist/`)
- `DllMain` → logger + pipe
- Teste: `tools/pipe_test.js` (Node). Sem app Electron neste repo.

**Pronto:** inject → pipe `connected`. Zero XTEA/Winsock no código.

## P1 — Self (leitura) — verde

HP, mana, cap, level, exp, posição, conditions, 10 slots. Qualquer char do mesmo `.exe`.

Pointer-chain → `LocalPlayer`; offsets no JSON do `buildId`; poll → `playerStats` + `inventory`.

**Pronto:** dois chars, mesmos números, sem recalibrar. Log: `pointer-chain resolved`. Posição no snapshot com o char no mapa.

## P2 — Containers (leitura) — verde

BPs **abertos** na memória (janela visível). Eventos `containerOpen` / `Update` / `Close`. Poll 250 ms.

O app (outro repo) guarda `index → {name, capacity, items}`. `useItem` em slot de BP é **P4**.

**Pronto:** abrir BP → itens na pipe; fechar → some. Verificado: backpack, locker, Inbox, dead rat (open/update/close).

## P3 — Battle (leitura) — verde

Criaturas visíveis (sem o LocalPlayer): `id`, `name`, `x,y,z`, `hpperc`, `kind`, `skull`, `inViewport`, `distance`.

Calibração (P1 pronto, monstro na tela): `node tools/p3-calibrate.js --name Rat`

Contrato: `docs/API.md`. O app mantém `id → creature`. `attack` / `follow` (P4) usam este `id` como `creatureId`.

**Pronto:** monstro na tela aparece na pipe; sai → some. Sem parser de pacote. Verificado (KarmaOT_GL): Rat/Bug no z=8; subir → `creatureDisappear` + Seymour no z=7; `kind` player/monster/npc; `inViewport` + `distance` Chebyshev; self fora da lista.

## P4 — Ações — verde

A pipe mexe no personagem, **sem** pacote XTEA: `say` → `step` → `use`/`move` → `attack` → `walkTo`.

Input na janela ou função do cliente. Contrato: `docs/API.md`. Descobertas de ABI: `configs/calibrations/README.md`.

Verificado no KarmaOT_GL (`"via":"client"`):

- `say` (`Game::talkChannel`)
- `walkTo` (`LocalPlayer::autoWalk`)
- `useItem` — Use e Use with (self, criatura P3, tile)
- `attack` / `cancelAttack` — sticky; `follow?` liga/desliga **chase** (`setChaseMode`), não `Game::follow`
- `follow` / `cancelFollow` — sticky; `Game::follow` sozinho **cancela** ataque
- `moveItem` — BP↔BP, inventário, chão; `count` omitido = pilha

`step` / `stop` por tecla (`"via":"keys"`).

**Pronto:** JSON na pipe faz o char falar, andar, usar, atacar (com ou sem chase), seguir e mover item. Verificado ao vivo.

## P5 — Mapa (leitura) — verde

Itens no tile em volta do player. Contrato: `docs/API.md`. Layout: bloco `map` no `{buildId}.json`.

Leitura de memória (`g_map` + blocos de `TilePtr` + vector de things). Sem opcode de mapa. Sem poll/eventos de tile. Calibração: `node tools/p5-calibrate.js [--id …]`.

Verificado no KarmaOT_GL: `getState what=map` devolve o chão sob o player (`items[0]`); andar 1 SQM e pedir de novo — a janela acompanha.

**Pronto:** `getState what=map` devolve o chão sob o player. Verificado ao vivo.

