# Ramonia x86 Research

Esta pasta concentra a pesquisa da build Ramonia x86 e é a base oficial para as próximas versões do probe.

## Fonte canônico

`src/RamoniaResearchProbe.cpp`

Apesar do arquivo histórico ter usado o nome `OTCLab_BuildMigrator_V4_9_DIRECT_BIND_x86.cpp`, o conteúdo atual corresponde ao **Research Probe V37**.

## Build rápido no Windows

Execute:

`build_x86.bat`

O script:

1. procura um compilador MinGW **x86/i686**;
2. recusa compiladores x64;
3. compila `src/RamoniaResearchProbe.cpp`;
4. gera uma pasta limpa em `build/Ramonia_x86_Test/`;
5. copia o profile canônico para o pacote de teste;
6. compila o injector x86 canônico de forma estática;
7. abre a pasta de teste ao concluir.

Saída esperada:

`build/Ramonia_x86_Test/RamoniaResearchProbe_V37.dll`

> A pasta `build/` é gerada localmente e não é versionada pelo Git.

## Estrutura

- `src/` — fonte canônico do probe atual.
- `profiles/ramonia_x86/` — profile persistente por fingerprint da build.
- `evidence/current/` — snapshots/resultados atuais não canônicos.
- `evidence/history/` — evidências antigas preservadas para comparação.
- `evidence/reference/` — manifests de referência offline.
- `tools/injector/` — injector x86 e dependência runtime usada nos testes atuais.
- `artifacts/legacy/` — DLLs/binários antigos preservados apenas como referência.

## Profile canônico

`profiles/ramonia_x86/6AB03F94_E281E9CA5BA0AF2.json`

Fingerprint:

- PE32 / x86
- timestamp `0x6AB03F94`
- SizeOfImage `0xAA2000`
- EntryPoint RVA `0x47F460`
- .text FNV1A64 `0xE281E9CA5BA0AF2`
- OpenGL

## Estado do probe

O source atual contém:

- descoberta PE/RTTI e semantic binds;
- validação de Game/Map owners;
- layouts de LocalPlayer, Creature, Inventory, Container, UIMap/MapView;
- Tile::things V30;
- Game Action ABI Static Probe V32;
- Game Action ABI Runtime Capture V33.1 + ABI Classifier V34;
- breakpoint temporário de um byte por ação;
- restauração do byte original no hit;
- `ReadProcessMemory` para snapshots do contexto;
- nenhuma chamada interna de ação feita pelo probe.

## Compatibilidade de profile

O serializer ainda grava o identificador legado `otclab-ramonia-x86-research-v27` / `profileVersion: 27`.
Isso **não representa a versão real do probe**. O parser atual valida a build por timestamp + hash e não usa esse número para aceitar/rejeitar o profile.

Não alterar esse metadado junto com mudanças funcionais. A migração de schema deve ser feita em commit separado e testada.

## Próximo passo

Usar este source como baseline e capturar as ações reais no V34 para classificar automaticamente os argumentos e persistir o bloco `actions` no profile. O probe continua sem executar chamadas internas.


## V35 — fechamento da ABI x86

O V35 mantém o breakpoint one-shot reversível da V33.1 e não chama ações internas por conta própria.

Ele fecha as ações que ainda estavam apenas `runtime_observed` usando evidência estrutural da própria chamada real:

- `autoWalk`: valida o vector de direções e seus elementos;
- `look/use/move/useWith`: reconhece referências a `shared_object_ptr` por stack-local holder + RTTI;
- `move`: reconhece `Position` OTC em layout compacto e o count escalar;
- ações já `runtime_confirmed` no profile são puladas automaticamente.

Quando executado dentro de `build/Ramonia_x86_Test`, cada save também espelha o profile para o caminho canônico `profiles/ramonia_x86/`. Assim o GitHub Desktop passa a mostrar o JSON atualizado sem cópia manual.


## V36 — fechamento de move.arg2

O V36 reaproveita todo o profile V35 e pula automaticamente as actions já `runtime_confirmed`. O foco é apenas o argumento de destino de `Game::move`.

A captura examina o argumento direto e os dois primeiros níveis apontados por ele, procurando os layouts de Position observáveis na build. Para um movimento chão -> chão, a promoção exige uma posição plausível e próxima da posição atual do LocalPlayer, reduzindo falsos positivos.

Se a classificação automática ainda não fechar, o console imprime um dump curto de bytes de `move.arg2`, `p0` e `p1` para a próxima análise sem repetir as outras actions.


## V37 — contrato principal Ramonia fechado

A evidência V36 mostrou `move.arg2` diretamente como `x=u32, y=u32, z=u8`:

`818, 1859, 7`, enquanto o LocalPlayer estava em `819, 1859, 7`.

O V37 incorpora esse layout ao classificador genérico, mantendo a validação pela posição atual do LocalPlayer antes de promover a ABI.

Contrato principal de actions do profile Ramonia:

- `autoWalk` → `vector_direction_u32_ref`
- `attack` → `Creature raw_ptr, bool Cancel` (`Cancel=false` confirmado para ataque real)
- `follow` → `Creature raw_ptr`
- `look` / `use` → `shared_object_ptr_ref`
- `move` → `shared_object_ptr_ref<Item>, Position(u32,u32,u8), u32 count`
- `useWith` → `shared_object_ptr_ref<Item>, shared_object_ptr_ref<Thing>`
- `talkChannel` → `MessageMode, Channel, msvc_string_ref`

O profile canônico alimenta agora a camada de runtime do Crome.

A bridge x86 V5 já confirmou `Game::attack` com dano real no jogo e expõe
`GET_VISIBLE_MONSTERS` + `ATTACK_ID`, mantendo ponteiros internos fora do IPC.

## Continuação guiada da descoberta x86

Depois de `[OWNER CLUSTERS]`, a probe oferece testes guiados da build atual,
sem depender da descoberta de LocalPlayer. Para cada método, mostra os
candidatos e pede a ação natural no jogo. Cada endereço pode receber amostras
A/B com objetos ou valores diferentes; o usuário pode pular e retomar depois.
TopMove e TopUse também entram nessa sequência. Endereços fora de seção
executável são rejeitados antes de instalar a captura temporária.

O JSON preserva todos os candidatos em `semanticBinds` e as capturas congeladas
em `guidedValidation`, incluindo bytes, thread e caller. A retomada exige o mesmo
fingerprint e conjunto atual de métodos/owners; nunca usa os ponteiros históricos.
Cada amostra é salva imediatamente. Pular um teste preserva suas evidências.

Uma captura de entrada não conclui a ABI: comparação tipada, retorno/cleanup,
ownership quando aplicável, dispatcher e chamada ativa continuam explícitos como
pendentes. Esses validadores completos ainda precisam ser implementados; a fase
guiada não chama funções internas nem habilita o Crome. Métodos sem candidato
são registrados como `discovery_required` e continuam na lista de pendências.

As buscas de hints ASCII agora leem blocos alinhados de até 4 KiB, preservando
strings entre blocos. Isso elimina as consultas de memória por byte após
`OWNER CLUSTERS`, inclusive em clientes com seções de recursos grandes.

### Guided Discovery R2

Nota: a revisão atual R3 substitui a fila por candidato descrita na R2 por
sessões por atividade. A busca semântica em blocos da R2 permanece.

O console identifica a DLL como `Guided Discovery R2` e imprime
`[BUILD REVISION] x86-guided-r2-batched-strings-all-candidates`.
A busca semântica indexa todos os nomes em uma passagem pelas seções legíveis,
incluindo `.rsrc`, com progresso a cada seção/16 MiB. Os resolvers de singleton e
instance reutilizam esse índice, sem repetir a varredura da imagem por nome.
Os padrões continuam sendo token exato + NUL, inclusive ocorrências sobrepostas;
um intervalo ilegível interrompe a continuidade do padrão e é contabilizado.

Cada método imprime todas as alternativas com método, owner, score, token e xref.
A fila guiada inclui também os métodos fora do contrato básico, priorizando os
ambíguos, depois os ausentes e então os candidatos únicos. Evidências desses
métodos adicionais também são retomadas no profile. O score permanece uma pista,
sem concluir a ABI. A revisão não implementa validação completa de retorno/refcount
ou chamadas ativas; essas etapas continuam explicitamente pendentes.

### Activity Sessions R3

Identificação: `[BUILD REVISION] x86-activity-r3-all-candidates-per-session`.
Após RTTI/semântica e obtenção do LocalPlayer, os campos básicos HP/Mana/XP/Level/
Capacity são calibrados antes de target, posição e centro do mapa. Depois vêm
sessões por atividade: status, andar, bags, itens, Look, Use, target, chat, luz e
outfit. Sem LocalPlayer, o observador ainda funciona; referências numéricas ficam
explicitamente indisponíveis. Descoberta detalhada adicional de layouts/ABI é
oferecida separadamente, depois das sessões, e não é obrigatória.

Cada sessão acompanha TODOS os RVAs executáveis dos métodos relacionados, com
deduplicação por endereço físico (máximo 512 endereços nesta revisão; excedentes
ficam pendentes). Não pergunta por candidato. Um endereço compartilhado por
vários métodos conserva suas observações em cada bind, sem atribuir sua classe
por score. A identificação de receptor RTTI é registrada por snapshot.

Fluxo: ENTER para preparar **2 segundos parado**, ENTER para preparar **10 segundos
da ação A**, ENTER para preparar **10 segundos da ação B**. Durante cada janela
o jogador permanece no jogo, sem retornar ao console. Bags A: abrir/fechar a bag;
B: outra bag, mover itens entre elas e arrastar a janela. Mover a janela não
garante uma chamada de Container; esse comportamento também fica registrado.
Andar A/B usa destinos/direções diferentes. O console mostra detecções, progresso
e `ATIVIDADE CONCLUIDA` ao terminar.

O observador usa INT3 reversível em cada candidato, uma captura por endereço
por rodada de 200 ms, renovada fora do handler. Não executa candidatos para
descobri-los. Por candidato/fase, conserva dois snapshots completos e o número
de rodadas com hit (não representa a quantidade total de chamadas). `guidedValidation`
inclui comparação controle/A/B, classes observadas, valores conhecidos do
LocalPlayer e posição antes/depois de cada fase. Campos ainda desconhecidos não
recebem offsets presumidos. Referências históricas são snapshots, nunca ponteiros
para chamada. Sem hit não descarta o candidato; hit isolado não conclui ABI.

Os resultados anteriores só são retomados para fingerprint e candidatos/owners
exatos. Métodos ainda sem candidato aparecem em uma lista, com uma opção única
para fornecer entradas encontradas no debugger desta build. O guia conserva
pendências dos métodos ainda sem uma atividade mapeada.

Ao finalizar, os bytes e proteções originais das páginas são restaurados. A DLL
permanece carregada sem breakpoints para tratar uma entrega atrasada de INT3
próprio já restaurado. Reinicie o cliente antes de atualizar/reinjetar a DLL.
`tests/GroupedObservationTests.cpp` testa 16 candidatos simultâneos, controle/A/B,
rearmamento concorrente, preservação de this/stack, proteção compartilhada e
ausência de promoção. A validação no cliente WAD ainda depende do teste real.

## R4: resultados claros no console

A tela principal mostra perguntas, tempo restante e resultados. Enderecos,
listas de candidatos, scores e snapshots ficam em `ResearchProbeDiagnostic.log`,
ao lado da DLL (renovado em cada execucao). O JSON continua preservando as evidencias.
Se o log nao puder ser aberto, os detalhes permanecem visiveis na tela.

Memorias basicas: CONFIRMADO quando houve validacao por mudanca em runtime;
IDENTIFICADO quando corresponde ao valor informado; PENDENTE quando falta
identificar/validar. Campos salvos exigem receptor local e leitura atual validos.

Funcoes: CANDIDATO quando uma alternativa foi observada nas duas acoes com
receptor esperado e mesma thread; INCONCLUSIVO quando ha alternativas, evidencia
insuficiente ou cobertura parcial; NAO OBSERVADO quando as acoes nao dispararam
o candidato; NAO TESTADO quando nao foi possivel observar. CANDIDATO nao conclui
ABI: retorno, argumentos, ownership/refcount e chamadas ativas continuam pendentes.
Nenhum desses resultados habilita `callable`. O final resume atividades e testes
pulados. Os testes de ConsoleSummaryTests cobrem classificacao, cobertura parcial,
receptor/thread, leitura atual e separacao entre tela e log.
