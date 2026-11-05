# Perfis e calibração por `buildId`

JSON por `buildId` do `.exe` (layout de memória). A DLL grava calibrações em
`<pasta da DLL>/calibrations/{buildId}.json`. Um arquivo externo válido tem
precedência; se faltar ou for inválido, a DLL carrega o perfil do mesmo `buildId`
incorporado durante o build a partir de `configs/calibrations/`.

O destino runtime das calibrações é único: `dist/calibrations/`. Os binários
finais ficam juntos em `dist/`, com sufixo
`-Win32` ou `-x64`; injector e DLL devem ter a mesma arquitetura do
processo. Não misture artefatos entre essas pastas. Não usa a pasta do jogo nem o
diretório corrente. JSON antigos junto do jogo são ignorados; para
migrar um perfil validado, copie-o manualmente para a pasta da DLL antes de injetar.
Falha de escrita é reportada, sem fallback para a pasta do jogo.

Compatibilidade por arquitetura é aditiva: suporte a um cliente x64 não autoriza
alterar a ordem, os limites ou a validação dos caminhos já verdes em Win32. O
calibrador tenta primeiro o layout sugerido pelo perfil; em seguida considera a
largura nativa de ponteiro e a largura de dois ponteiros necessária por um
`shared_ptr`. Em Win32, a ordem histórica de 8 e 4 bytes permanece; em x64, 16
bytes passa a ser uma alternativa de descoberta. A largura encontrada pertence
ao novo `{buildId}.json` e nunca vira condição por servidor no C++.

`configs/calibrations/` é o catálogo fonte de perfis aprovados. O build incorpora
todos os `pe-*.json` desse catálogo à DLL e também copia somente perfis ausentes
na Release, preservando calibrações novas. O JSON salvo por uma DLL já carregada
não altera seus próprios bytes; após fechar P1–P5, promova e recompile:

```powershell
tools/embed-calibration.ps1 -BuildId <buildId> -Architecture Win32
tools/embed-calibration.ps1 -BuildId <buildId> -Architecture x64

npm run calibration:embed:win32 -- --BuildId <buildId>
npm run calibration:embed:x64 -- --BuildId <buildId>
```

O comando valida o `buildId`, copia o perfil ao catálogo, recompila o destino
normal, força a regeneração do catálogo C++ mesmo quando o JSON copiado tem data
antiga e confirma que o identificador está presente na DLL produzida.

### Distribuição

Depois da promoção e do rebuild, o pacote consumidor pode usar somente
`OTCApi-Win32.dll` ou `OTCApi-x64.dll`; `calibrations/*.json` não é dependência de execução. O injector e o
app que consome a pipe continuam sendo artefatos separados quando necessários.
Não há caminho absoluto da máquina do calibrador gravado no pacote.
Perfis só se aplicam ao mesmo PE/buildId, com restore comprovado após restart;
uma atualização do cliente pode exigir nova calibração. Calibrar tenta escrever
na pasta da DLL; consumir um perfil incorporado não exige o diretório externo nem
permissão de escrita.

Offsets **não** saem na pipe. Perfil = layout da **build**, não do personagem. Contrato público: `docs/API.md`.

### Estado do catálogo e reset

Em 02/09/2026, o usuário solicitou zerar as calibrações. O catálogo fonte está sem
JSON; oito arquivos de perfis/cópias foram removidos dos locais de carga após backup
verificado, mantido fora do Git. O JSON Valantis recalibrado permanece local na Release.
As seções de Karma/DeusOT abaixo registram evidência histórica, não arquivos hoje incluídos.

Para resetar uma build: feche o jogo e os consumidores da pipe, faça backup do JSON
da DLL e remova somente esse arquivo. Se houver o mesmo perfil em `configs/calibrations/`,
um rebuild poderá repô-lo; retire-o do catálogo apenas se o reset for intencional.
Reabra/injete e faça P1 completo antes de `--refine`. Não apagar todos os JSON do
projeto nem renomear perfis de outro PE. A DLL antiga já carregada continua com
o perfil em memória até encerrar o processo.

Registro de mudanças e evidências: [notas de 02/09](../../docs/CALIBRATION-NOTES-2026-09-02.md).

Não hardcodar estes fatos no C++. Ficam no JSON; o scan tem que redescobrir em outro `.exe`. Os números de RVA abaixo são **deste** KarmaOT_GL Win32 (`pe-407f0b0e-…`); outro cliente muda.

## Fluxo completo para um `.exe` novo

Este é o runbook canônico para KarmaOT, DeusOT ou qualquer cliente futuro. O nome do servidor serve apenas para escolher o processo no injector; identidade e compatibilidade vêm exclusivamente do `buildId` emitido pela DLL.

### 0. Preparar processo, Release e pipe

1. Identifique a arquitetura do cliente e compile no destino correspondente:
   Os artefatos finais ficam em `dist/`; abra o cliente usando o par da arquitetura correta.
   injector e DLL desse mesmo destino.
2. Entre no personagem e permaneça no mapa antes de começar o P1.
3. Execute jogo, injector, calibradores e Monitor no **mesmo nível de privilégio**. `OpenProcess ... err=5` ou `EPERM` na pipe normalmente significa que o jogo está elevado e a outra ponta não.
4. A pipe aceita um cliente por vez. Feche o Monitor enquanto roda `tools/*-calibrate.js`; feche o calibrador antes de abrir o Monitor.
5. Injete a DLL recém-compilada. Se o injector disser `already loaded`, reinicie o processo do jogo antes de validar: DLL já carregada não é substituída em memória.

```powershell
dist/OTCInjector-Win32.exe --name <Cliente.exe>           # Win32
dist/OTCInjector-x64.exe --name <Cliente.exe>             # x64
```

O primeiro `dllStatus` informa o `buildId`. Com a DLL injetada, execute
`node tools/pipe_test.js` e copie o valor completo de `data.buildId`. Para listar
perfis já salvos, use `Get-ChildItem dist/calibrations/*.json`. Não renomeie um
perfil antigo para esse id. Confira `calibrations/{buildId}.json` ao lado da DLL
efetivamente injetada; o log registra o caminho usado. A pasta do jogo não tem override.

### 1. P1 — localizar LocalPlayer com todos os valores exatos

Leia os números **atuais** da UI imediatamente antes do comando. Em clientes com layout misto, informar HP, mana, level, experiência e capacidade reduz falsos positivos e permite demonstrar o encoding de cada campo.

```powershell
node tools/p1-calibrate.js --health <atual> --maxHealth <max> --mana <atual> --maxMana <max> --level <level> --exp <exp> --cap <cap-livre>
```

Aceite somente `ok:true`, `chainSteps > 0`, `statsBase:true` e os números corretos em `playerStats`. `chainSteps=0` é uma cópia de stats, não uma âncora restaurável; o calibrador se recusa a salvá-la. `HP copy ignored` também é rejeição intencional. Não afrouxe validações globais para transformar uma cópia em LocalPlayer.

Ao repetir o P1 completo para o mesmo `buildId`, a nova cadeia e o layout de stats
substituem os anteriores, mas position/states/inventário e P2–P5 já calibrados são
preservados. Dados nunca são transportados entre `buildId` diferentes.

`chainSteps>0` é necessário, mas não suficiente: até uma cadeia de cinco passos
pode funcionar só naquela sessão. Após refine de posição/inventário, faça também
um restart de controle P1 antes de investir nas demais etapas. Se ocorrer recuperação
automática, repita o restart e exija resolução direta da cadeia corrigida.

### 2. P1 refine — posição, states e inventário

Equipe um item cujo **client id** e slot sejam conhecidos. Para descobrir states, use `--battle` somente enquanto o ícone/swords de battle estiver ativo. Se souber a posição completa, passe `--x`, `--y` e `--z`. Se souber apenas uma região, passe os quatro limites inclusivos `--minX/--maxX/--minY/--maxY` junto de `--z`; só `--z` continua válido.

```powershell
node tools/p1-calibrate.js --refine --x <x> --y <y> --z <z> --inventory --id <client-id> --slot <1-10>
node tools/p1-calibrate.js --refine --minX <x-min> --maxX <x-max> --minY <y-min> --maxY <y-max> --z <z>
node tools/p1-calibrate.js --refine --z <z> --battle
```

Os limites são evidência temporária do wizard, não layout do cliente: eles filtram candidatos durante o refine, mas não são persistidos no `{buildId}.json`. Isso permite reutilizar, por exemplo, o mesmo retângulo de uma cidade em executáveis novos sem criar branches por servidor. Sem os quatro limites, o caminho anterior permanece inalterado.

Se a posição calibrada não acompanhar um movimento de 1 SQM, mova o personagem e repita com
`--moved`. Esse modo força a redescoberta e exclui o offset já comprovado como cópia estática:

```powershell
node tools/p1-calibrate.js --refine --z <z> --moved
```

Quando só o `z` é conhecido e ainda houver cópias estáticas, use `--moving` e caminhe durante os
quatro segundos da coleta. Só um campo cujo X/Y mudou mantendo o mesmo Z será aceito:

```powershell
node tools/p1-calibrate.js --refine --z <z> --moving
```

Não há corte regional implícito para X/Y: toda a faixa `0..65535` é válida. Os quatro limites
`minX/maxX/minY/maxY` são opcionais e só restringem o scan quando informados pelo operador.

O refine com `--id/--slot` só salva depois de ver esse item no snapshot daquele slot. Isso separa um array real de dez slots de linhas de inteiros parecidas no objeto.

### 3. P2 — container aberto

Abra a janela da bag/backpack, deixe pelo menos um item dentro e informe a quantidade de slots da **janela**, não a capacidade do personagem. `--id` é opcional, mas fortalece a validação.

```powershell
node tools/p2-calibrate.js --name <nome-da-janela> --capacity <slots> --id <client-id-opcional>
node tools/pipe_test.js --only containers
```

Confirme `containerOpen`, alteração de item em `containerUpdate` e fechamento em `containerClose`.

Uma bag fechada pode continuar na memória por referências Lua. Se o P2 encontrar
itens mas retornar `no pointer-chain to container collection`, nome e conteúdo
não comprovam que o objeto pertence às janelas abertas. Após falhar o caminho
habitual, a descoberta tenta um `std::map` no módulo usando todos os candidatos
com o nome informado. Exige árvore coerente, chaves iguais aos índices dos
containers, capacidade e itens válidos. Entradas nulas são permitidas, mas uma
bag vazia sozinha não inicia a calibração. Depois de calibrado, esvaziar ou fechar
a janela continua sendo um estado válido. Não preencher o RVA manualmente.

### 4. P3 — criatura conhecida

Mantenha uma criatura visível no mesmo andar. Não é necessário atacá-la. O comando
não presume o HP; passe `--hpperc` quando souber o valor exato visível. Se a barra estiver
cheia. O HP exato é obrigatório para não confundir campos enum 0–3 com vida.
`--id` é opcional quando o id já apareceu em diagnóstico. Se o X/Y do P1 ainda não
estiver comprovado, passe o andar conhecido com `--z`: o P3 usa somente esse andar
como filtro e não confia no X/Y provisório.

```powershell
node tools/p3-calibrate.js --name Rat [--z <0-15>] [--hpperc <0-100>] [--id <creature-id>]
node tools/pipe_test.js --only creatures
```

Confirme `creatureAppear`, atualização e desaparecimento, sem o LocalPlayer na lista.
Em coleções `shared_ptr`, entradas com control block expirado não podem aparecer no
snapshot. IDs `0x40000000..0x7fffffff` são monstros por faixa e não devem virar
`unknown` por causa de um campo auxiliar inconsistente.

### 5. P4 seguro e âncoras do mapa

Não existe wizard P4: cada ação resolve seus Lua binds e salva os RVAs no mesmo perfil. Comece por ações reversíveis e observe o efeito no cliente, não apenas `ok:true`.

```powershell
node tools/p4-test.js --say calibration-test
node tools/p4-test.js --walkTo <x> <y> <z>
node tools/p4-test.js --look <x> <y> <z>
node tools/p4-test.js --useItem <container> <slot>
node tools/p4-test.js --moveItem <container> <slot> --toItem <container> <slot>
```

O `look` em um SQM é um caminho seguro para resolver `g_map`, `Map::getTile` e as funções de look antes do P5. Depois do P3, inspecione `creatures.creaturePtrSize`: em builds de ponteiro cru/intrusivo (`4` no Win32), `attack` não nulo exige a rota por `EventDispatcher::addEvent`; a chamada direta pela WndProc continua proibida. Essa rota foi validada no Valantis `pe-61beeb3a-…`. `follow` raw continua sem rota validada e falha fechado; não grave RVA como “verde” só porque o bind foi localizado.

### 6. P5 — mapa

Com P1/posição e `actions.mapRva` prontos:

```powershell
node tools/p5-calibrate.js
node tools/p5-calibrate.js --id <client-id-no-tile-do-player>
node tools/pipe_test.js --only map
```

Tente primeiro sem `--id`; use o hint quando o tile for ambíguo. Ande um SQM e repita o estado: a janela deve acompanhar o personagem e continuar no mesmo `z`.

### 7. Restaurar, promover e testar no Monitor

1. Feche o jogo, abra novamente o **mesmo exe**, injete sem hints e exija `dllStatus.phase:"ready"` + `pointer-chain resolved` no log.
2. Compare `player`, `inventory`, `containers`, `creatures` e `map` com a UI. Uma restauração em endereço de heap diferente é a prova de que o perfil não salvou endereço absoluto.
3. Copie o perfil validado de `dist/calibrations/{buildId}.json` para `configs/calibrations/{buildId}.json` e confira que as duas cópias são iguais. O rebuild não sobrescreve perfis já existentes em `dist`.
4. Feche qualquer calibrador e abra o Monitor no mesmo nível de privilégio do jogo.
5. Depois de qualquer alteração em scan/read/action C++, revalide ao vivo um `buildId` previamente verde. Se o executável não estiver disponível, registre a lacuna; build e compatibilidade estática não equivalem a regressão live.

### Critérios de aceite

| Área | Evidência mínima |
|------|-----------------|
| P1 | valores exatos, `chainSteps>0`, posição correta e restart sem scan |
| Inventário | 10 slots e o item-hint no slot informado |
| P2 | open/update/close da janela realmente aberta |
| P3 | appear/update/disappear, mesmo `z`, self ausente; HP corresponde à barra conhecida |
| P4 | ação visível no cliente; ABI/ownership demonstrada para aquele tipo de ponteiro |
| P5 | tile do player não vazio e janela acompanhando após mover 1 SQM |
| Pacote | JSON fonte e cópia da Release com o mesmo conteúdo |

## DeusOT — build atual calibrado para leitura

Perfil histórico: `pe-a6b7516c-6a8eddb3-01ba3000-0121a200-014c.json`, validado ao vivo em 2026-08-27 e retirado dos locais de carga no reset de 02/09. Estes números descrevem **somente esse PE** e não devem virar defaults no C++.

| Área | Descoberta desse build | Estado |
|------|------------------------|--------|
| P1 | `Game` image-relative → LocalPlayer em `Game+0` → stats; `double` para HP/mana/level/cap, `uint64` para experiência; `capacityScale=1` | leitura validada e restaurável |
| Posição/states | posição `int32` relativa ao bloco de stats; states separado | validado no mapa |
| Inventário | dez slots de `Item*` cru, largura 4; id/count no objeto Item | validado |
| P2 | `std::map`, Container* cru, items em `std::deque`, largura 4; Bag de 8 slots/1 item usada como evidência | validado |
| P3 | `std::map`, Creature* cru, largura 4; id/nome/posição/hp% no objeto | validado com Rat visível |
| P5 | blocos `std::map` 32×32; `TilePtr` e things com largura 4; vector de things em offset 36 | validado com e sem item hint |
| P4 seguro | say/walk/look/use/useWith/move e âncoras de map têm RVAs no perfil | caminhos calibrados; validar cada efeito no app |
| `attack`/`follow` | `CreaturePtr` intrusivo cru; chamada direta de alvo não nulo reentra callbacks/dispatcher | **não verde**; RVAs ficam `0`, falha fechada |

O perfil anterior `pe-a9376da6-6a885df0-01b9c000-01215000-014c` corresponde a outro PE do DeusOT e contém somente o fechamento P1 daquela build. Nunca o reutilize ou renomeie para a build atual.

### Diagnóstico rápido

| Sintoma | Ação |
|---------|------|
| `OpenProcess ... err=5` / pipe `EPERM` | alinhar privilégio de jogo, injector, Node e Monitor |
| `already loaded` | reiniciar o processo exato para carregar a DLL recém-buildada |
| `scan found no chained LocalPlayer (... HP copy ignored)` | confirmar personagem no mapa e hints exatos; não salvar cópia de HP |
| `chainSteps=0` | rejeitar e repetir P1; não é restore estável |
| `hasChain:true`, `statsBase:false` após restart | conferir no log carga do JSON e falha de cadeia; não apagar nem refazer tudo sem diagnóstico |
| `LocalPlayer recovered; profile chain repaired` | reiniciar de novo e exigir `pointer-chain resolved` sem hints |
| recuperação com `profile save failed` | corrigir permissão da pasta da DLL; a recuperação em memória não prova persistência |
| `container-chain landed but read failed` | diagnóstico do layout/cadeia P2; a versão atual corrige o último deref do caminho via heap |
| `container-chain did not resolve to the discovered collection` | candidato P2 rejeitado por resolver outro endereço; não salvar manualmente |
| inventário não encontrado | manter item equipado, informar client id + slot e repetir refine |
| container não encontrado | abrir a janela, deixar item dentro e conferir nome/capacidade de slots |
| criatura não encontrada | mesmo `z`, nome visível; adicionar `--hpperc` se não estiver 100% |
| P5 pede `actions.mapRva` | fazer `p4-test --look x y z` e confirmar o look no cliente |
| Monitor não conecta | fechar calibrador/outro consumidor e alinhar privilégio |
| ação de criatura raw falha | comportamento seguro atual; não repetir nem preencher RVA manualmente |

Restauração P1: a cadeia salva continua sendo tentada primeiro. Só se falhar,
perfis com posição e inventário de ponteiros calibrados e offset final de stats
podem usar a referência Game → LocalPlayer identificada por metadados Lua. O membro
é buscado entre 0 e 64 bytes com alinhamento nativo, não fixado em Game+0; o deslocamento
encontrado fica na cadeia do perfil. O offset final de stats deve ser positivo, até 8192.
O fallback não faz heap scan: exige vtable, leitura exata da posição pelo layout,
ponteiros de equipamentos válidos, stats plausíveis, candidato único e confirmação
em dois polls separados. Preserva os demais blocos e salva apenas a nova cadeia.
Isso não substitui o teste de restart: a próxima injeção deve resolver a cadeia
salva diretamente. Falhas de acesso ao diretório da DLL são reportadas no status.

Valantis `pe-61beeb3a-…`: recuperação observada em PID 4472 com Game RVA `10776304`,
membro `+4` e stats `+1200`; PID 1216 restaurou diretamente em outro endereço de heap.
Depois, o log confirmou calibração P2 (`map`, uma janela), P3 e P5. Isso ainda não
comprova restart dessas três etapas nem corrige o erro anterior de coordenadas do look.

### Variações de encoding no P1

O perfil pode descrever campos que não usam o mesmo encoding do restante do bloco:

| Campo JSON | Efeito | Compatibilidade quando omitido |
|------------|--------|-------------------------------|
| `experienceType: "uint64"` | lê `experience` como inteiro de 64 bits mesmo quando `statType` é `double` | usa o comportamento histórico ditado por `statType` |
| `capacityRounding: "truncate"` | trunca a capacidade depois de aplicar `capacityScale` | arredonda para o inteiro mais próximo, como nos perfis antigos |

`totalCapacity`, `magicLevel` e `soul` são opcionais dentro de `fields`. Quando o scan não consegue demonstrar um offset, ele omite o campo do perfil e a pipe publica `0`; nunca reutiliza silenciosamente o offset de outro atributo. Perfis antigos que já contêm esses campos mantêm exatamente a leitura anterior.

## P1 — LocalPlayer

Inject → personagem no mapa:

```
node tools/p1-calibrate.js --health … --level …
node tools/p1-calibrate.js --refine --z 7 --battle --inventory
```

### O que o P1 mostrou (MSVC OTClient / KarmaOT_GL, Win32)

| Fato | Detalhe |
|------|---------|
| Stats | `double` (`statType`), `capacityScale` 100 |
| Chain | 1º passo: deref do **control block** do `LocalPlayer` (`shared_ptr` `_Rep`); 2º: `+1272` no bloco = stats no objeto (`LocalPlayer+1256` neste build). Não tratar o 1º ponteiro como `T*`. Outro `.exe` pode ter `Game` no heap: o scan tenta **depois** o 1-deref com vtable (este layout) um 2-deref a partir de ponteiros **graváveis** no módulo (`.data`, não `.rdata`). |
| Posição | `int32` x,y,z (`positionLayout: int32`), offset relativo ao stats base (aqui negativo: objeto Creature atrás dos stats). Runtime: se o offset P1 falhar, a DLL lê o tile no objeto LocalPlayer (mesmo layout P3) e guarda o offset vivo. |
| Inventário | array de 10 `ItemPtr` **8 bytes**, `itemIdOffset` 48, `itemCountOffset` 52 (iguais aos itens de container). |
| `g_game` | RVA no bloco `actions.gameRva`. LocalPlayer T* em `Game+0`. |

## P2 — Containers

Mesmo `{buildId}.json`, bloco `containers`. Janela **aberta** com ≥1 item:

```
node tools/p2-calibrate.js --name Backpack --capacity 20
```

- `--name` ignora maiúsculas. Neste cliente a RAM guarda `backpack` mesmo com a janela `Backpack`.
- `--capacity` = slots da grade (20, 30, 5…), não a cap do char.
- Equipada no inventário sem abrir a grade **não** basta.
- Se o P3 deste `.exe` já estiver no JSON, o scan procura a lista **perto** de `knownCreatures` / LocalPlayer (não só `std::map` no heap).

Contrato: `containerOpen` / `Update` / `Close` + `getState what=containers`.

### O que o P2 mostrou (MSVC OTClient / KarmaOT_GL, Win32)

| Fato | Detalhe |
|------|---------|
| `ItemPtr` | **8 bytes** (`inventory.ptrSize` / `containers.itemPtrSize`), não 4 — tipo `shared_ptr`. Usar o stride do P1. |
| Coleção | `std::map` **ou** `std::unordered_map` de containers abertos **no módulo** (`.data` do Game). Outro `.exe` pode ser `umap`. |
| Itens | `std::deque` MSVC, `dequeBlockSize` 2 (porque o elemento tem 8 bytes) |
| Nome | `std::string` MSVC (`nameKind: msvc`); comparar sem case |
| Âncora | objeto Container no heap; offsets relativos (`id` 16, `capacity` 20, `name` 32, `items` 72 neste build) |
| Destino vazio (P4 `move`) | tile especial `{x:0xFFFF, y: containerId\|0x40, z: slot}` — `idOffset` 16 é esse id de janela. Inventário vazio: `{x:0xFFFF, y: slot, z: 0}`. Item ocupado: copiar `Thing::m_position` (`+0x10/+0x14/+0x18`). |

Scan que assume ponteiro de 4 bytes **descarta** a BP mesmo vendo o texto do nome. Primeira calibração falhou por isso + `Backpack` vs `backpack`. Scan que só procura `std::map` falha se o Game guardar `unordered_map` (comum em OTClient).

No fallback de coleção via heap, os dois passos são dereferenciados: módulo →
objeto intermediário → coleção. Antes de aceitar o perfil, a cadeia deve resolver
exatamente à coleção descoberta, que ainda precisa passar pela leitura de containers.
As cadeias de coleção embutida no módulo e de ponteiro direto permanecem inalteradas.

## P3 — Battle / criaturas visíveis

Mesmo `{buildId}.json`, bloco `creatures`. Personagem no mapa (P1) e **um** monstro visível no mesmo andar:

```
node tools/p3-calibrate.js --name Rat
```

- `--name` ignora maiúsculas e aceita trecho (`Spider` acha `Poison Spider`).
- `--z` (0–15) permite calibrar pelo andar conhecido quando o X/Y do P1 ainda não foi comprovado.
- `--hpperc` aceita 0–100, é opcional e nunca presume 100; quando usado, informe o valor exato da barra. `--id` é opcional (creature id do cliente).
- Sem `--z`, precisa da posição do player (P1 `--refine --z`) uma vez por `.exe`.
- A lista na pipe **não** inclui o LocalPlayer.
- Monstro sem `m_name` no objeto (nome só no ThingType): o scan usa a lista P3 já gravada e grava `typePtrOffset`.

Contrato: `creatureAppear` / `Update` / `Disappear` + `getState what=creatures`. Campos: `id`, `name`, `x`, `y`, `z`, `hpperc`, `kind`, `skull`, `inViewport`, `distance`.

### O que o P3 mostrou (MSVC OTClient / KarmaOT_GL, Win32)

| Fato | Detalhe |
|------|---------|
| `CreaturePtr` | **8 bytes** (`creatures.creaturePtrSize`, igual ao `ItemPtr` do P2) — `shared_ptr`. |
| Coleção | `std::map` de criaturas conhecidas **no módulo** (`.data` do Game), chain **sem deref**. Perto dos containers (~+1312 bytes neste build), não é o mesmo mapa. |
| Nó do map | key `uint32` (creature id) em offset 16; `T*` do `shared_ptr` em **24** (4 bytes de padding após a key). Não usar a key como ponteiro. |
| Nome | `std::string` MSVC (`nameKind: msvc`) neste cliente. |
| `m_id` | 4 bytes **imediatamente antes** do nome (`nameOffset - 4`). Ids de player nesta OT começam em `0x10000000` e sobrepõem heap Win32 — classificar `kind` pela faixa OTClient, não por “parece ponteiro”. |
| Posição | `int32` x,y,z no objeto (não packed). |
| `hpperc` | `uint8` (0–100). Campo `int32` pequeno (1–3) no objeto **não** é porcentagem. |
| `kind` | Faixa do id: player `0x10000000…0x3FFFFFFF`, monster `0x40000000…0x7FFFFFFF`, npc `≥ 0x80000000`. |
| LocalPlayer | Muitas vezes **não** está em `knownCreatures` (templo). A pipe **omite** o self de propósito; corte/`distance` usam `playerStats.position` (P1, com fallback no tile do próprio objeto). |
| Viewport | Mapa desenhado ~8×6 SQM; lista até 12 SQM no mesmo `z`. `distance` = Chebyshev. |

Scan que trata id de player como ponteiro de heap marca o char (e outros players) como `kind: unknown`. Chave de mapa que é endereço de heap **não** é `m_id`.

## P4 — Ações

Não há wizard P4: a DLL resolve as funções do cliente (Lua bind) na primeira ação e grava RVA no bloco `actions` do mesmo `{buildId}.json`. Teste: `node tools/p4-test.js …`.

Bloco `actions` (RVA relativas à imagem do `.exe`):

| Campo | Função |
|-------|--------|
| `gameRva` | `g_game` |
| `talkChannelRva` | `Game::talkChannel` |
| `autoWalkRva` | `LocalPlayer::autoWalk` (não `Game::autoWalk`) |
| `useRva` / `useWithRva` | `Game::use` / `Game::useWith` |
| `attackRva` / `followRva` | `Game::attack` / `Game::follow` |
| `chaseModeRva` | `Game::setChaseMode` |
| `moveRva` | `Game::move` |
| `mapRva` / `getTileRva` / `topUseRva` | `g_map`, `Map::getTile`, `Tile::getTopUseThing` (Use with no tile) |
| `lookRva` | `Game::look` (evento `look` + ação `look`) |
| `ambientLightRva` / `ambientLightGetRva` | `UIMap::setMinimumAmbientLight` / getter; `setLight` local |

`setLight` valida que setter e getter MSVC Win32 acessam os mesmos membros antes
de chamar. O objeto UIMap é localizado por RTTI e precisa apontar para MapView;
a busca de memória é limitada a 4 segundos. Os RVAs são descobertos no bind e
persistidos por buildId; o brilho anterior fica apenas em memória para restauração.
Valantis `pe-61beeb3a-…`: ligar com `1` e restaurar `0` validados em 03/09, com
confirmação visual. Nenhuma equivalência de ABI é presumida em outros clientes.
TibiaScape x64 `pe-55ddf475-…`: os binds usam referências RIP-relative; os
accessors MSVC x64 foram aceitos somente após ambos indicarem os mesmos offsets de
UIMap e MapView. No RTTI x64, o TypeDescriptor dentro do COL é RVA de 32 bits,
enquanto `vftable[-1]` é um ponteiro nativo de 8 bytes para o COL. A busca de heap
prioriza a vizinhança do LocalPlayer para continuar encontrando a UI após sua
recriação e ainda exige RTTI `UIMap`/`MapView`. Dois ciclos on/off restauraram
visualmente 1→0 com `ok:true`; RVAs 1504544/1505216 estão no perfil da build.

Scan: string Lua (`"attack"`, `"useWith"`, `"setChaseMode"`, `"look"`, …) → xref → ponteiro da member. Pular wrappers (`cancelAttack`, `useWith` quando se procura `use`, `moveRaw`, `moveToParentContainer`).

Em x64, o fallback pode decodificar `LEA reg,[RIP+disp32]` para localizar o nome
do bind, o Game inline e o método. A seção executável deve ser validada uma vez e
percorrida diretamente; validar memória byte a byte fez a pipe expirar no teste
inicial do TibiaScape. A build `pe-55ddf475-6a82c258-00fdb000-009d6200-8664`
validou somente `say` ao vivo (`via:"client"`). Cada outra ação ainda exige prova
separada do efeito e da ABI antes de persistir seu RVA como suportado.
O mesmo se aplica ao bind `autoWalk`: em x64, tanto o xref da string quanto os
alvos próximos podem ser RIP-relative. Falhar a descoberta deve manter
`autoWalkRva=0`; só promover o RVA após `walkTo` mover o personagem ao vivo.
No TibiaScape citado, o reteste moveu o personagem e persistiu
`autoWalkRva=1453952`. O helper geral de binds (`look` e outras ações) também
precisa decodificar xrefs e alvos RIP-relative no x64; uma falha deve manter o RVA
da ação em zero até o efeito correspondente ser confirmado no cliente.
Para `getTile` no MSVC x64, o segundo campo do `TilePtr` fica em
`+sizeof(void*)` (`+8`), não em `+4`. O Complete Object Locator x64 também guarda
o TypeDescriptor como RVA de 32 bits; converter pelo image base antes de validar
RTTI. O caminho x86 continua usando o ponteiro absoluto e stride de 4 bytes.
No TibiaScape x64, o reteste após essa correção executou o tile `look` e exibiu o
resultado no cliente. Isso valida também as âncoras usadas nesse caminho; RVAs de
outras ações encontrados durante o scan continuam pendentes até seus efeitos
serem observados separadamente.
O P5 possui sua própria validação de RTTI em `map_reader.cpp` e deve aplicar a
mesma conversão de TypeDescriptor RVA no MSVC x64. Execute primeiro sem `--id`;
quando necessário, `--id` recebe o client item id de 16 bits observado no tile,
e não um endereço, handle ou creature id.
Cada execução substitui o hint P5 anterior: sem `--id`, o script envia
`mapItemId:0` para limpar o filtro persistente da DLL. Valores fora de 100–59999
são rejeitados antes da conexão.
No TibiaScape x64 citado, a execução seguinte sem `--id` retornou tiles coerentes
no mesmo andar ao redor de 32096,32204,7, marcou `hasMap:true` e salvou o layout
`map-blocks`. Uma leitura posterior sem recalibrar encontrou o player em
32092,32203,7 e devolveu a janela correspondente de raio 8 (`x=32084..32100`,
`y=32195..32211`), comprovando que o mapa acompanha o movimento. Ainda é preciso
reiniciar o mesmo PE e exigir leitura sem hints antes de promover o JSON.
Esse restart foi concluído no PID 22536: a pipe entrou diretamente em
`phase:"ready"` e restaurou player, inventário, Bag, criaturas e o mapa em torno
de 32097,32204,7, sem executar P1–P5. O perfil DLL-local foi então copiado para o
catálogo fonte com conteúdo e SHA-256 idênticos. Isso torna a leitura P1/P2/P3/P5
distribuível para esse PE; ações sem confirmação visual individual não ficam
automaticamente verdes.

### O que o P4 mostrou (MSVC OTClient / KarmaOT_GL, Win32)

| Fato | Detalhe |
|------|---------|
| Thread | Chamar na WndProc da janela do jogo. **PostMessage**, não SendMessage — `useWith` / `attack` / `follow` / `move` / `autoWalk` falam com o dispatcher e penduram se a mensagem for síncrona. |
| `Game` | `+0` LocalPlayer T*; `+8` attacking T*; `+0x10` following T*. |
| `CreaturePtr` / `ItemPtr` by value | thiscall, `T*` + `_Rep`, callee dá `ret 8`. Incrementar `_Rep+4` (uses) **antes** de `attack` / `follow`. |
| `Game::move` | `ThingPtr&` + `Position&` + `int count`, `ret 0xC`. **Não** bump de uses (é referência). `count <= 0` vira 1 no cliente; a pipe, se omitir `count`, passa a pilha lida no objeto. |
| `Game::use` | `ItemPtr` 8 bytes. `Game::useWith(item, thing, subType=0)`. |
| Position | `int32` x@0, `int32` y@4, `uint16` z@8 (12 bytes). `autoWalk` recebe por valor. |
| `Map::getTile` | thiscall, `Position*`, **EAX = slot de TilePtr** no array do sector (`lea eax,[eax+esi*8]`), não `Tile*`. Sentinel da imagem = falha. Desembrulhar T*/`_Rep`; RTTI tem que conter `Tile`, não `_Ref_count`. |
| `Tile::getTopUseThing` | thiscall, hidden `ThingPtr*` retorno, `ret 4`. `this` = `Tile*`. Fallback: vector de things em `Tile+0x28`. Passar o EAX cru do `getTile` como `this` **crasha**. |
| Sticky | `Game::attack` / `follow` no mesmo alvo **toggle off**. A pipe, se `Game+8` / `Game+0x10` já é esse T*, **não** chama de novo. Cancel = ptr vazio. |
| Chase vs follow | `Game::follow` **cancela** o ataque. `"follow":true` em `attack` é `setChaseMode(ChaseOpponent=1)` depois `attack`. `false` = `DontChase=0`. Omitido = não mexe. Comando `follow` continua só seguir. |
| `walkTo` | `autoWalk` no **objeto LocalPlayer** (`T*`), não em `g_game`. Sem caminho: `ok: false`, `error: "no path"`. |
| Look | Lua `"look\0"` → `Game::look`. thiscall, `const ThingPtr&` + `bool isBattleList`. A DLL põe um JMP no início para emitir o evento `look` no Shift+click (e na ação). Thing `m_position` em `+0x10`. Inventário/container: `x=0xFFFF`. |
| Click | Mesmo `getTile` + `getTopUseThing` do Use with no tile, depois `Game::use` no Thing (não `useWith`). Ação `click` `x,y,z`. |

Neste build (exemplo, não copiar para outro exe): `talkChannelRva=612912`, `useRva=625136`, `useWithRva=626352`, `attackRva=482480`, `followRva=514656`, `chaseModeRva=597792`, `moveRva=549648`, `autoWalkRva=1361840`, `getTileRva=1405104`, `topUseRva=1679952`, `gameRva=12011368`, `mapRva=12012584`.

## P5 — Mapa / tiles (leitura)

Mesmo `{buildId}.json`, bloco `map`. Personagem no mapa (P1) e `actions.mapRva` (P4):

```
node tools/p5-calibrate.js
node tools/p5-calibrate.js --id 351
```

- `--id` é opcional: client id de um item no **tile do player** (chão ou drop).
- O scan pode chamar `Map::getTile` **uma vez** só para achar o `Tile*`. Runtime do `getState` só lê RAM.
- Contrato: `getState what=map` → array `{x,y,z, items:[{id,count}]}`. Sem eventos de tile. Raio Chebyshev no JSON (`radius`, default 8), mesmo `z`.

### O que o P5 mostrou (MSVC OTClient / KarmaOT_GL, Win32)

| Fato | Detalhe |
|------|---------|
| Âncora | `g_map` (`mapRva` = `actions.mapRva`, 12012584 neste build). `tileBlocksOffset` **0**: o primeiro membro é `m_tileBlocks[16]`. |
| Coleção | `std::map` por andar (`map-blocks`), como P2/P3 — não `unordered_map`. Stride **8** (head + size). 16 andares. |
| Bloco | `blockSize` 32. `TilePtr` **8 bytes** embutido no nó (`tilesVector` false). Índice `(x%32)+(y%32)*32` (`xy`). |
| Chave | OTClient `getBlockIndex`: `(y/32)*2048+(x/32)` (`keyOrder: otc`). **Não** `(bx<<16)|by`. |
| Nó | key `uint32` em **16**; array em **20** (sem pad de 4 — o `CreaturePtr` do P3 fica em **24**). Slot do `getTile` fica mod 8 = 4. |
| Tile | sem x,y,z no objeto (`posOff=-1`). Coordenada = chave + índice. |
| Things | `std::vector` em `Tile+40` (`0x28`, igual ao fallback P4). Id/count 48/52 como P1. |
| Pipe | só Item; chão primeiro. Vazio omitido. Raio Chebyshev 8, mesmo `z`. |

Scan que trata o EAX de `getTile` como `Tile*` (sem unwrap) não acha o array do sector. `blockSize` e `thingsOffset` no C++ **não**; ficam no JSON. Verificado ao vivo: chão sob o player e janela acompanha 1 SQM.

### Validação da posição P3

Na posição de Creature, `0xffff` é sentinela inválida. Quando P1 e LocalPlayer estiverem disponíveis, o candidato P3 precisa reproduzir a posição P1 no objeto do jogador. O filtro de visibilidade usa a âncora P1 independente, para que um offset P3 provisório não esconda uma coleção válida após restart.

Creature ids identify one live lifecycle. After `creatureDisappear`, clients must
discard that id even if a creature with the same name remains visible. For manual
P4 validation, `node tools/p4-test.js --attackAt x y z` requests the current P3
snapshot and resolves the id on that exact tile immediately before `attack`; it
fails closed when the tile is empty or ambiguous.

For MSVC `shared_ptr`, the strong counter follows the native-width vptr:
`_Rep+4` on Win32 and `_Rep+8` on x64. Balance a by-value temporary at
`_Rep+sizeof(void*)`. A call returning without an exception is not action success;
for attack/cancel, verify `Game::m_attackingCreature` reached the requested pointer
or null before reporting `ok:true`.

Do not infer the x64 CreaturePtr method arity from Win32. Inspect call sites. In
the TibiaScape PE, `Game::attack` consumes `R8B`, and a true value branches past
the protocol send. An empty CreaturePtr represents cancellation; API attack and
cancel must both pass false so the state reaches the server. Internal wrappers
may pass true for a local-only update. `Game::follow` remains a separate
two-argument ABI.

Live validation on this TibiaScape PE confirmed that normal attack starts chase
and server-side combat, and that an empty target with the boolean false stops the
server-side attack. This validates `attack`, chase mode, and `cancelAttack` for
this build only; independent `follow` still requires its own live check.

### Ordem do inventário

Inventory slots follow the OTClient enum: 1 head, 2 necklace, 3 backpack, 4 armor, 5 right hand, 6 left hand, 7 legs, 8 feet, 9 ring, 10 ammo. Confirm the UI position before providing `--slot`; a valid item id at the wrong slot can calibrate an array shifted by one element.

