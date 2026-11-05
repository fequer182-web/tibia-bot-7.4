# OTCLab ABI Builder V1.3 X64 — AutoCalibrator / Caller Evidence

O padrão do pipe exige o JSON recém-gerado pelo BuildExplorer atualizado, com `pipeDiscovery` e fingerprint exata. Profiles legados sem esse catálogo não autorizam captura. Os testes não habilitam funções no runtime (`callable=false`). Veja [fluxo e pendências](../PIPE_DISCOVERY.md).

A etapa inicial observa ações naturais. A revisão Completion V1 acrescenta testes de retorno, ownership e chamadas ativas guiadas, oferecidos separadamente. O jogador fornece os estímulos pedidos; não são usados ponteiros históricos em chamadas.

## O que a V1.3 adiciona

Além dos snapshots de `RCX/RDX/R8/R9` da V1.2, cada hit agora tenta resolver automaticamente:

- `returnRva`;
- `naturalCallSiteRva`;
- tipo do CALL (`call_rel32` ou `call_indirect`);
- início/fim da função caller usando a exception directory (`.pdata`) do PE x64;
- janela de bytes ao redor do CALL;
- prefixo da função caller;
- consistência do mesmo CALL site entre estímulo A e B.

Tudo é gravado no `abiCatalog` schema v4 do mesmo JSON.

## Regra de promoção

`callerCandidateReady=true` somente quando a ABI foi validada, a thread natural é consistente e o mesmo CALL site apareceu nos dois estímulos A/B. `callable` continua **false**.

## Uso

1. Copie para `profiles\` o JSON mais novo salvo pelo BuildExplorer atualizado, contendo `pipeDiscovery` e a fingerprint exata do cliente.
2. Rode `build.bat`.
3. Injete `Build\OTCLab_ABIBuilder_V1_3_X64_AUTOCALIBRATOR_CALLER_EVIDENCE.dll`.
4. Faça as ações pedidas normalmente.
5. No resumo, procure linhas como `caller site -> 0x... validated=YES`.
6. Envie o console completo e o JSON atualizado.

Profiles antigos sem o catálogo de descoberta atual são recusados. Evidências ABI anteriores só são preservadas com a mesma fingerprint e o mesmo RVA redescoberto.

## Diagnóstico de fechamento ao injetar

A DLL grava `ABIBuilderStartup.log` ao lado dela assim que a thread de trabalho inicia. O arquivo inclui etapas da inicialização e uma cópia do console. A ausência do arquivo pode significar falha antes dessa thread ou falta de permissão de escrita; não prova uma causa específica.

O console usa handles próprios do Windows, sem `freopen` e sem alterar `stdin`, `stdout`, `stderr` ou os handles padrão do cliente. Seus buffers C++ são restaurados antes de descarregar. O build também incorpora a dependência de threads do compilador, evitando carregar `libwinpthread-1.dll` do cliente ou do injector.

Para testar, abra uma sessão nova do Olders, sem Crome/BuildExplorer/outras probes carregadas, e injete somente esta ABIBuilder. Primeiro confirme que o console chega à pergunta inicial. Se fechar, envie as últimas linhas do log. Compilação e testes locais não confirmam a resolução de um crash no cliente real.

Os testes `tests/StartupIoTests.cpp` verificam leitura/escrita guiada, preservação dos streams do host, restauração dos buffers inclusive após exceção e ausência de breakpoint na inicialização. As funções permanecem `callable=false`.

Os caminhos de DLL e profiles permanecem em UTF-16 nativo do Windows durante busca, leitura, gravação e backup. A apresentação no console usa UTF-8. `tests/UnicodeProfileTests.cpp` cobre pastas como `Área de Trabalho` e nomes de arquivo com acentos, sem depender da configuração de locale do cliente.

## Look normal e detailedLook

O assistente testa primeiro o candidato `Game::look` redescoberto para a build atual, pois o estímulo pedido é o Look normal. Se não observar nenhuma chamada, testa `Game::detailedLook` separadamente, quando também descoberto. Uma captura parcial não é combinada com outra função. A chave do catálogo e o RVA reutilizado por Move/Use/UseWith são os da função realmente capturada. Um caminho de Item ambíguo mantém o assistente pendente; `callable` permanece falso.

`tests/LookSelectionTests.cpp` cobre seleção normal, fallback separado, identidade do catálogo/cache e rejeição de captura parcial ou descoberta de outra build.

## Seleção de ações por build: Look e Talk

O catálogo compartilhado `common/PipeDiscoveryCatalog.hpp` define as famílias sem RVAs: Look normal/detalhado e Talk de canal/normal. O BuildExplorer exporta essa política em `pipeDiscovery.actionSelection`, junto dos candidatos buscados no executável atual. A ABIBuilder usa o mesmo seletor para as duas famílias; perfis atuais anteriores a esse campo adicional continuam aceitos.

O teste de chat pede duas frases no mesmo canal Default. Testa primeiro `Game::talkChannel`, quando redescoberto de forma única, e depois `Game::talk` somente se o primeiro não tiver chamadas observadas. Capturas parciais permanecem pendentes. O catálogo salva a chave e RVA da função observada; evidências de TalkChannel não validam Talk, nem o contrário. Capturar texto não confirma os parâmetros de canal/modo, ownership ou chamada ativa.

Strings curtas temporárias são verificadas nos bytes capturados na entrada, evitando depender da pilha do caller após o usuário pressionar ENTER. `tests/TalkSelectionTests.cpp` cobre esse caso, os dois caminhos de seleção, rejeição de descoberta ambígua/outra build, threads diferentes e preservação de `callable=false`.

## Correspondências fortes e candidatos próximos

O JSON mantém `evidenceCandidates` completo e `candidateReview`, com ranking, diferença de pontuação e indicação de empate/próximos. Pontuação é uma heurística de comparação, nunca prova da ABI. Candidatos próximos pedem um novo estímulo que varie apenas um argumento; todas as alternativas continuam registradas.

Cada snapshot inclui os bytes medidos em `bytesHex`. `frozenCandidateReplay` compara caminhos de registradores/wrappers nos próprios snapshots, sem consultar endereços antigos do processo; ausência de bytes fica explícita. O limite de oito capturas persistidas por estímulo é declarado em `captureStorage`, inclusive quando houve truncamento. `captureScope` identifica a origem histórica e não autoriza reutilizar ponteiros em chamadas.

`validationProgress` separa evidências medidas de contrato completo, retorno, ownership, dispatch e teste ativo ainda pendentes. Ao retomar a mesma fingerprint e mesmo RVA redescoberto, o console revisa os candidatos salvos. `lastEvidenceSession` agora conserva as últimas observações de cada função pulada, mesmo inconclusivas, enquanto `functions` preserva a melhor evidência. Mudança de fingerprint/RVA invalida a observação correspondente. Os testes `CandidateEvidenceTests.cpp` verificam esses casos.

A revisão de dados históricos continua passiva. Depois dela, a etapa Completion V1 executa os validadores descritos abaixo.

## Próximo estágio

Depois de CALL site/caller estáveis, o estágio seguinte pode aprender **como o caller constrói cada argumento antes do CALL**, ainda passivamente, e persistir essas fontes no JSON para automatizar migrações entre builds.

## Completion V1: testes que faltavam

Depois do resumo inicial, confirme `Executar os testes de conclusao pendentes agora?`.
O Builder lista todos os candidatos da fingerprint atual, incluindo alternativas
ambíguas, e pergunta quais testar. Os cinco primeiros grupos são oferecidos por
padrão; os demais são opcionais. Falta de hit/caller/estrutura permanece explícita.

- **TopMove/TopUse:** pareia entrada e retorno no mesmo thread e frame; reconhece
  armazenamento de retorno na pilha do caller, confere o objeto no mesmo
  `Tile::things`, identifica candidatos a refcount e repete a ação com watchpoints
  de escrita. Exige incremento `+1` e liberação pelo caller até o valor inicial,
  em dois objetos diferentes, com offset único. Depois oferece chamada ativa
  usando o Tile e armazenamento da invocação atual; o caller original libera
  esse resultado. Baixamento diferente de `RCX=Tile/RDX=storage` permanece pendente.
- **Game::autoWalk:** dois cliques em linhas retas livres, com quantidade de SQMs
  informada; verifica vetor de direções de 1 ou 4 bytes, posição inicial e Game,
  retorno consumido como bool e consistência do thread. Depois oferece um clique
  ativo e pede confirmação do movimento. Outros layouts/consumidores de retorno
  ficam pendentes, em vez de receber um tipo presumido.
- **Item::getId/getCount:** verifica candidatos cujo corpo seja exatamente uma
  leitura escalar de campo seguida de RET. Confere offset/largura com o profile
  e chama esse getter em dois Items atuais com valores diferentes, no thread do
  Look natural. Corpos complexos não são chamados por esse teste.
- **Ações Game:** oferece relay dos argumentos atuais quando houver evidência
  de entrada/caller e descarte do retorno. Registra efeito e thread, mas marca
  `replayOnly=true`: isso não valida construção independente de holders, strings,
  modo/canal ou ownership. Nunca libera uma função por esse relay.
- **Demais getters:** registra entrada/retorno pareados em dois estímulos e
  conserva as pendências de receptor tipado, representação e ownership. Ainda
  não há validadores completos para todos os getters de Map/Container/Position.

Os resultados incrementais ficam em `abiCompletion` no mesmo JSON, separados
do catálogo passivo. `contractComplete` exige todas as etapas do validador;
`selectedRva` só aparece após testar o conjunto inteiro e obter um único contrato
completo. Uma mudança de fingerprint ou conjunto de candidatos invalida a
reutilização. `callable=false` e `runtimeAdapterReady=false` permanecem até existir
um adaptador independente no runtime do Crome; concluir o teste guiado não instala
esse adaptador. O Crome e o Cavebot não são alterados nesta revisão.

Capturas são limitadas a 30 mil instruções/150 ms no thread interceptado. Um
timeout ou ausência de release não prova ownership. INT3 e registradores de debug
são restaurados; a DLL mantém um handler sem hooks armados até fechar o cliente
para reconhecer uma entrega atrasada de INT3 já restaurado. Portanto encerre e
reabra o cliente antes de substituir/reinjetar esta revisão. ENTER encerra a sessão
e fecha o console, mas não força descarregamento desse handler.

`tests/CompletionTraceTests.cpp` executa uma função real de laboratório x64,
medindo entrada/retorno, escrita de incremento, liberação pelo caller e chamada
ativa sem duplicação. Também verifica cancelamento sem hit, INT3 atrasado,
breakpoints alheios, leitura escalar e rejeição de vetor/refcount inválidos.
Esses testes não substituem a validação no Olders da fingerprint correspondente.

## Instrucoes da prova final x64

Cada um dos 29 testes de conclusao mostra um nome simples, o significado da
funcao e um roteiro especifico para A e B. A/B sao duas amostras; os enderecos
candidatos sao hipoteses que recebem o mesmo roteiro. O usuario nao precisa
identificar qual endereco esta correto. GetTile consulta um SQM; GetContainers
consulta bags abertas; Container::getCapacity e quantidade de slots, nao peso.
As acoes sugeridas estimulam a interface e nao garantem chamada em toda build.
Ausencia de captura permanece inconclusiva. Roteiros nao alteram contratos ABI,
selecao de candidatos, ownership ou callable.
