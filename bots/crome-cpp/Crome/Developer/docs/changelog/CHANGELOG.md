# Crome — Changelog consolidado

## v7.12
- Corrigido Auto Fishing: o runtime agora recarrega `fishing.json` em tempo de execucao; antes o checkbox podia alterar o JSON depois que a DLL ja havia carregado a configuracao, deixando Fishing efetivamente OFF ate reiniciar o runtime.
- Fishing mantem o caminho nativo validado `Game::useWith(rod, waterThing)`, procura a vara em equipment/container, usa IDs de agua configurados, sorteia entre tiles validos e aplica delay variavel.
- `Save Profile...` e `Load Profile...` agora usam o seletor nativo de arquivos do Windows; nao e mais necessario digitar o nome do profile.
- Profile v2 inclui Cavebot/waypoints, Loot/DropItem, Walker IDs, Fishing e Tools em um unico JSON.
- Adicionado `config/tools.json` como contrato persistente de Food, Anti Idle, Mana Shield, Anti-Paralyze, Auto-Haste, Light, Spear Pickup e ReAmmo.
- Follow permanece opcional/OFF por padrao; nao faz parte do requisito do Cavebot.
- Light permanece gated por profile; nao e habilitado genericamente no Olders.
Arquivo único de histórico. Cada nova versão deve ser adicionada abaixo, mantendo as anteriores para facilitar regressões e comparação.

---

# Crome v7.1 - Etapa Cavebot/Targeting

## Alterado dentro de Developer

- `ui/main.cpp`
  - todos os módulos/opções agora iniciam desmarcados;
  - Healing Light/Medium/Heavy/UH inicia OFF;
  - Rune Maker/Trainer inicia OFF;
  - Cavebot, Targeting, Keep Follow, Attack Any Monster e Should Attack Me iniciam OFF;
  - Tools, Loot e Alarms iniciam OFF;
  - restaurado campo `Monsters` e `Mode` no Targeting do Cavebot;
  - `Add` agora abre editor para adicionar node manual: Walk, Stand, Use, Rope, Shovel, Ladder, Hole, Say e Wait;
  - `Record` virou liga/desliga. Enquanto ligado grava deslocamentos automaticamente;
  - movimento no mesmo andar gera `Walk x,y,z`;
  - mudança de Z gera `FloorChange x,y,z`;
  - Record NÃO inventa Rope/Shovel/Ladder: essas classificações ficam pendentes até termos o observador de eventos/actions;
  - ajuste de altura/scroll para não cortar os controles novos.

- `modules/ModuleConfig.hpp`
  - adicionado `WaypointType::FloorChange`.

- `modules/targeting/TargetingEngine.cpp`
  - `Attack Any Monster=false`: só aceita criaturas presentes nas regras/lista;
  - nomes comparados sem diferenciar maiúsculas/minúsculas;
  - prioridade da lista é da esquerda para a direita;
  - `Attack Any Monster=true`: libera qualquer criatura válida e usa distância para desempate.

- `config/targeting.json`
  - defaults todos OFF e lista vazia.

- `config/healing.json`
  - todas as regras OFF por padrão; UH incluída explicitamente.

- `config/waypoints.json`
  - Cavebot e Record OFF por padrão e lista de nodes vazia.

## Não marcado como pronto nesta etapa

- execução nativa de Attack/Follow/Use/Talk continua dependente da validação de ABI/thread do profile;
- Record ainda não consegue distinguir automaticamente Rope vs Shovel vs Ladder apenas pela mudança de posição;
- CreatureReader completo ainda precisa alimentar Targeting para o teste real de `rat`.

---

# Crome v7.2 - Healing pass 1

## Alterado em Developer/

- `ui/main.cpp`
  - UH deixou de ser texto/spell editavel.
  - Healing ganhou master enable.
  - Spell rows: HP %, Mana %, Min ML, spell e enable.
  - Nova secao de Items com UH, Mana Fluid e Life Fluid.
  - IDs de UH/Mana Fluid/Life Fluid sao editaveis pelo usuario.
  - UH usa trigger HP <=; Mana Fluid usa MP <=; Life Fluid usa HP <=.
  - Todos os toggles continuam OFF por padrao.

- `modules/ModuleConfig.hpp`
  - HealingConfig agora separa spell rules de self-use items.
  - Campos dedicados para IDs e thresholds de UH/Mana Fluid/Life Fluid.
  - Spell rule passou a usar Mana % e Min Magic Level.

- `modules/healing/HealingEngine.cpp`
  - Corrigido bug antigo: o campo Mana % agora compara com `manaPercent()` e nao mana absoluta.
  - UH -> `useWithGround(itemId, player.position)`.
  - Mana Fluid -> mesma action de self position, disparada por MP %.
  - Life Fluid -> mesma action de self position, disparada por HP %.
  - Spell healing continua via `say(spell)`.
  - Cooldowns separados para spell/item/fluid.

- `config/healing.json`
  - Novo formato com `spellRules` e `items`.
  - UH default ID 2273 por ser o exemplo/documentado no material Rifbot.
  - Mana Fluid e Life Fluid ficam ID 0 para o usuario configurar por build/servidor.

## Status de runtime

A leitura de HP/Mana/posicao ja esta ativa. A logica de decisao desta versao esta implementada.
A execucao efetiva de `say` e `useWithGround` depende da action nativa da build estar ligada ao backend. O profile Olders atual ainda marca nativeActionsReady=false, portanto o projeto nao deve fingir que a chamada nativa foi validada.

---

# Crome v7.3 Consolidated

## Correções consolidadas
- Healing voltou ao layout compacto do Rifbot.
- Removida a coluna Min ML do Healing.
- Mana das magias é valor absoluto: `HP <= 95% && Mana >= 25`, não porcentagem.
- UH/MF/LF ficam em uma única linha; IDs configuráveis pelo botão `IDs`.
- Defaults permanecem OFF.
- Mantidos campo Monsters/Attack Any Monster, Add x Record, player snapshot real e profile detection das versões v7.1/v7.2.
- Rodapé não usa mais o rótulo genérico `Native actions: BLOCKED`. Para Olders mostra a evidência dos testes antigos por ação.

## Evidência de runtime já obtida no Olders
Logs anteriores do Crome SDK registraram chamadas no game/window thread para:
- `LocalPlayer::autoWalk` RVA `0x1805C0`: retorno `bool=true` e progresso do personagem.
- `Game::attack` RVA `0x165410`: retorno normal e AttackTarget confirmado.
- `Game::use` RVA `0x164570`: retorno normal.
- `Game::useWith` RVA `0x164A50`: usado em testes F12/shovel/rope/fishing.
- `Game::talk` RVA `0x165D40`: caminho de direct talk e Healing usados nas builds SDK V21+.

## Importante
O código-fonte do dispatcher/action bridge das DLLs V21/V22 não está dentro do pacote Crome v7.x atual. Por isso este pacote corrige UI/regra/config e preserva a evidência de que as ações já foram comprovadas, mas não finge que o bridge externo foi recomposto sem esse fonte. O próximo passo é portar o dispatcher das DLLs antigas para `core/actions/` ou recuperar o fonte/ZIP V21/V22.

---

# Crome v7.4 Native Bridge

## Base usada
- Crome Universal v7.3 Consolidated
- Nostale_Bot_CromeSDK_Integration_v21_6_FIXED.zip (runtime funcional enviado pelo usuario)
- BuildProfile/reader atual do Crome

## Alteracoes

### Runtime nativo restaurado
- Adicionado `Developer/native_runtime/` com o dispatcher e actions da V21.6.
- Novo target `CromeRuntime.dll`.
- Crome.exe injeta automaticamente `CromeRuntime.dll` no `Olders_OpenGL.exe` quando a build esta aberta.
- A DLL executa as chamadas no game/window thread, como na V21.6 validada.

### Healing
- HP continua em porcentagem.
- Mana de Light/Mid/High e valor absoluto (ex.: 25 = 25 mana, nao 25%).
- UI compacta.
- Light/Mid/High sincronizam para `config/healing.json` e o runtime recarrega automaticamente.
- UH/Mana Fluid/Life Fluid usam `item_on_self` via Game::useWith -> LocalPlayer e IDs editaveis.
- Defaults OFF.

### Targeting
- Campo Monsters sincroniza para o runtime.
- Attack Any Monster OFF = somente lista.
- Attack Any Monster ON = qualquer monster valido.
- Targeting ON/OFF e demais valores sao recarregados automaticamente pela DLL.
- Game::attack volta a usar o caminho V21.6.

### Cavebot
- Crome grava `config/waypoints.json` diretamente no schema da V21.6.
- Enabled ON inicia o Walker automaticamente quando ha nodes.
- Record de Walk/FloorChange vira node de walk executavel.
- Add manual converte Walk/Wait/Say/Use/Rope/Shovel para nodes nativos.
- LocalPlayer::autoWalk volta a usar o dispatcher V21.6.

### Build/Distribuicao
`build.bat` agora gera e copia:
- `Crome/Crome.exe`
- `Crome/CromeRuntime.dll`
- `Crome/profiles/builds.json`
- configs nativos necessarios

Configs anteriores sao salvos como `*.backup.json` antes da instalacao dos defaults.

## Validacao feita aqui
Os testes de logica recuperados da V21.6 foram recompilados e passaram:
- Healing condition/cooldown: OK
- Targeting sticky/chase config: OK
- Walker say/tool nodes: OK

A execucao real dentro de `Olders_OpenGL.exe` precisa ser confirmada no PC do usuario, pois o cliente nao existe neste ambiente.

---

# Crome v7.5 Inspect

## Mudancas
- Healing reduzido para 140 px e coluna principal sem scroll global. O cabecalho/coracao nao some mais ao rolar.
- Runtime injeta no maximo uma vez por PID. Se a DLL nao carregar, nao entra em loop; existe Retry manual no rodape.
- CromeRuntime nao abre mais console preto. O log vai para `CromeRuntime.log`.
- Se o dispatcher falhar durante a inicializacao, a DLL permanece carregada e tenta novamente internamente, sem unload/reinjecao.
- Novo `Look/Inspect` no rodape Runtime/Build: le os IDs do tile informado e permite copiar/classificar como Ladder/Rope/Shovel.
- `items.json` armazena os IDs classificados sem duplicar.
- Node Ladder agora e gravado no waypoints.json, procura o item pelo ID no tile, chama Game::use e aguarda mudanca de Z antes de seguir.
- Walk, Healing/Talk, Attack e filtro de Target foram preservados.

## Limite atual do Look
A v7.5 ainda nao intercepta automaticamente o evento nativo de Look do cliente. O botao Inspect le o tile informado (preenchido inicialmente com a posicao atual do player), suficiente para descobrir o ID real sem adivinhar.

---

# Crome v0.7.6 - Global ID Catalog

## Alteracoes
- Novo `config/walker_ids.json` global, compartilhado por todas as builds/OTs.
- Categorias recuperadas do Rifbot: Ladder, Rope, Shovel, Pick, Mine Pick, Machete, Pathable, Non Pathable, Open Holes, Counter, Depot, Food, Rock, Corpse Bag, alem de Clear Before Use e Fishing.
- IDs numericos pre-carregados apenas quando havia evidencia: 1948 (ladder observado no teste), 3003 (rope), 3457 (shovel), 3483 (fishing rod).
- Botao `IDs` no Cavebot abre editor compacto das listas globais.
- `Inspect` pode salvar rapidamente ID como Ladder/Rope/Shovel/Pathable/Clear.
- Recorder melhora FloorChange: se o ultimo tile inspecionado estiver perto e o ID estiver catalogado, grava Ladder/Rope/Shovel em vez de FloorChange generico.
- Tool IDs do route JSON agora usam o primeiro ID salvo no catalogo global.

## Importante
O material do Rifbot confirma as categorias e o fluxo de Look -> Information Box, mas nao expôs uma lista numerica completa de defaults. IDs ausentes foram deixados vazios; nao foram inventados.

## Teste
1. `build.bat`
2. Abra Olders e Crome.
3. Cavebot -> `IDs`: confira 1948/3003/3457/3483.
4. Adicione outros IDs identificados e clique `Save IDs`.
5. Grave um floor change com um ID conhecido previamente inspecionado; o recorder deve classificar o node quando houver evidencia suficiente.

---

# Crome v7.7 Runtime Guard

## Alterado
- Removidos do rodape os controles Inspect / Copy ID / Ladder / Rope / Shovel / Path / Clear.
- Rodape voltou a ser somente Runtime / Build / Player / Native status.
- Quando o runtime nativo nao carrega, Crome mostra uma janela obrigatoria com `Retry Runtime` e mantem os modulos nativos bloqueados.
- Sem reinjecao automatica infinita: o retry continua manual por PID.
- Cavebot mostra `WAITING RUNTIME` quando o backend nativo nao esta disponivel e grava `enabled=false` no JSON.
- Healing, Rune Maker, Tools, Loot e Targeting ficam desabilitados visualmente enquanto Native != CONNECTED.
- Janela `IDs` agora e o unico lugar de gerenciamento do catalogo global.
- `Ultimo Look / leitura` aparece somente dentro da janela IDs; nenhuma classificacao e feita automaticamente.
- `Edit` de waypoint agora abre editor real: tipo, X/Y/Z, Item ID, texto ou delay conforme o node.
- Record continua usando o catalogo para reconhecer Ladder/Rope/Shovel/Open Hole quando existe uma leitura de tile conhecida; caso contrario grava FloorChange generico.

## Preservado
- Runtime v21.6 e dispatcher.
- Healing/Talk confirmado.
- Walker/walkTo confirmado.
- Targeting/Game::attack e filtro de monstros confirmados.
- `walker_ids.json` global para todas as builds.

---

# Crome v7.8 Look ID

## Alterações
- Captura experimental do evento nativo `Game::look` no Olders profile confirmado.
- O monitor valida os 15 bytes iniciais do `Game::look` antes de instalar o hook. Se a assinatura não casar, ele não altera a função.
- Ao dar Look em um item, grava `config/look_result.json`.
- Item em ground/mapa: grava `id + x/y/z`.
- Item em container/inventory: grava apenas `id`.
- O rodapé mostra somente `Look ID` e posição quando aplicável.
- A janela `IDs` voltou a ser somente um editor manual do catálogo global.
- `walker_ids.json` continua global e não é sobrescrito pelo build.
- Healing, Walker, Targeting e Runtime Guard foram preservados.

## Teste
1. Abra Olders e entre no personagem.
2. Abra Crome e confirme `Native: CONNECTED`.
3. Dê Look em um item no chão. O rodapé deve mostrar `Look ID: <id> Pos: x,y,z`.
4. Dê Look em um item de backpack/container. O rodapé deve mostrar `Look ID: <id>` sem posição.
5. Abra Cavebot > IDs e copie manualmente o número para a categoria desejada.

## Segurança de runtime
O hook só é instalado no profile Olders quando o endereço `0x163790` contém o prólogo conhecido do `Game::look`. Em mismatch, o monitor é desativado e o restante do runtime continua funcionando.

---

# Crome v0.7.9 — x64 Look ID + HackLight profile contract

- Adicionado `features.look` e `features.hackLight` aos profiles OTCLab oficiais.
- Classico Look confirmado: `Game::look RVA 0x164090`, `Thing = *(RDX)`, Item ID `+0x40`.
- Classico HackLight confirmado: `LocalPlayer +0xF1` (`u8`), transicao `0 -> 6 -> 8 -> 0`, escrita/restore validados; valor testado `15`.
- Olders Look corrigido para o caminho validado: `Game::detailedLook RVA 0x163B10`, Item ID `+0x38`, ground pos `+0x18/+0x1C/+0x20`, container sentinel `X=65535`.
- Olders HackLight continua `pending`; nenhum offset foi adivinhado.
- TibiaScape Look continua `candidate` e HackLight `pending` ate teste runtime.
- Adicionados dois probes de calibracao em `Developer/tests/x64_features/`.
- `Developer/build.bat tests` compila o release e depois os dois testes de feature.
- `ProfileLoader` expoe Look/HackLight tipados para os modulos do bot consumirem o JSON em vez de offsets hardcoded.

---

# Crome Universal v7.10 - Save/Load, Lootbag, Fishing

- Cavebot: Save Hunt / Load Hunt em `Crome/hunts/hunt.json`.
- Profile: Save Profile / Load Profile em `Crome/profiles/user/<nome>.json`; bundle inclui bot/healing/targeting/loot/waypoints/walker_ids/fishing.
- Loot: configuracao por IDs e nova lista DropItem para lootbag. O contrato esta pronto; a execucao DropItem fica bloqueada enquanto `Game::move` nao estiver validado no runtime daquela build.
- Fishing: Auto Fishing em loop, usa `fishingRod` e `fishingWater`, escolhe um tile de agua valido pseudo-aleatorio a cada uso e aplica delay randomico entre min/max. Pausa quando Walker/Targeting estao ativos.
- Follow Player: quando Follow manual esta ativo e Targeting esta OFF, o runtime pausa o Walker e restaura quando Follow termina, evitando disputa de movimento. Quando Targeting liga, o Follow do jogo continua bloqueado como protecao.
- Classico: o Crome pode abrir o profile e testar a camada de leitura sem exigir o runtime Olders; acoes nativas continuam dependentes de validacao por build.

---

# Crome v7.11 — Look corrigido, Profile único e Fishing equipamento+bag

- Removidos os botões separados `Save Hunt / Load Hunt`; `Save Profile / Load Profile` passa a ser o fluxo recomendado e inclui o Cavebot/waypoints.
- O carregamento de Profile agora força a UI a recarregar `waypoints.json`, `targeting.json`, `healing.json`, `loot.json`, `walker_ids.json` e `fishing.json`, evitando que os valores antigos da UI sobrescrevam imediatamente o Profile carregado.
- Olders Look corrigido no runtime: o monitor passa a usar o caminho confirmado `Game::detailedLook RVA 0x163B10`, com `Thing* = *(RDX)` e Item ID `Thing+0x38`.
- A assinatura do hook de Look foi alinhada ao probe v1.5 que funcionou no Olders.
- Fishing agora procura a vara primeiro nos slots de equipamento e depois nos containers abertos.
- Corrigido o bug que zerava `water_ids`: a UI montava `fishing.json` a partir de `walker_ids.json`, mas `fishingWater` vinha vazio; o catalogo agora inicia com 4597-4602 usados no teste e continua editavel por OT.
- Olders inventory runtime configurado em `LocalPlayer +0x550`, stride 8, 10 slots.
- Fishing continua em loop, seleciona água aleatoriamente e evita repetir o mesmo candidato consecutivamente quando houver mais de um tile válido.
- Adicionado aviso visual: `Keep Follow/chase` pode disputar movimento com Cavebot; manter OFF quando não for necessário perseguir.
- `DropItem -> Ground` e IDs continuam salvos no Profile; execução nativa segue condicionada à validação do `Game::move` da build.
- Changelog consolidado em `Developer/docs/changelog/CHANGELOG.md`; changelogs soltos antigos foram removidos.
