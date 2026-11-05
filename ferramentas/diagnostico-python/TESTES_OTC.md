# OTC — monitor e testes externos

As duas leituras de capacidade sao preservadas, com seus offsets e escalas. No seu perfil sao `+0x6D0` e `+0x6D8`, ambos `float64` com escala 100. Aparecem separadamente como Capacidade 1 e Capacidade 2. As telas de Status e Equipamento explicam a utilidade de manter ambas; ainda nao atribuimos automaticamente qual offset corresponde a cada tela.

## Preparar

Extraia os arquivos juntos em `Cromin_py`. O pacote inclui `Antica_monitor.profile.json`, uma copia do perfil salvo pelo Builder Livre.

```powershell
Set-Location "C:\Users\Usuario\OneDrive\Documentos\Cromin_py"
python -m pip install psutil
```

Use Python de 64 bits. Abra o cliente e entre no personagem. Nao precisa reutilizar o PID antigo: o programa procura o nome do cliente. Se houver varias instancias, acrescente `--pid NUMERO`.

## 1. Monitorar atributos

```powershell
python .\otc_monitor.py --profile .\Antica_monitor.profile.json --count 20
```

Mostra HP, experiencia, as duas capacidades, posicao, direcao e os ponteiros de ataque/seguir. Os campos pulados permanecem sem leituras. Campos ambiguos sao mostrados com esse estado. Mova o personagem pelo jogo e altere normalmente itens para comparar as leituras com as duas telas. O monitor nao executa essas acoes.

## 2. Testar ataque e seguir

```powershell
python .\otc_monitor.py --profile .\Antica_monitor.profile.json --test both
```

O teste de ataque pede tres estados:

1. Sem ataque/seguir: cancele no jogo e pressione ENTER no programa.
2. Atacando: ataque uma criatura pelo jogo, mantenha o alvo e pressione ENTER no programa.
3. Cancelado: cancele o ataque no jogo e pressione ENTER no programa.

Depois repete o procedimento com Seguir, usando a opcao do proprio jogo, sem atacar. Se a criatura desaparecer antes de capturar o estado ativo, o resultado pode ser inconclusivo; repita o teste. Digite `pular` em qualquer etapa para pular aquele teste.

O resultado `observed_target_transition` exige observar `0 -> ponteiro plausivel -> 0`. Plausivel aqui significa que o alvo tem um ponteiro inicial para uma vtable dentro do modulo e que a primeira entrada aponta para `.text`. Essa e uma verificacao estrutural simples, nao prova a classe concreta do alvo.

Tambem pode executar somente um teste:

```powershell
python .\otc_monitor.py --profile .\Antica_monitor.profile.json --test attack
python .\otc_monitor.py --profile .\Antica_monitor.profile.json --test follow
```

## O que este teste valida

Observa os slots de estado do jogo associados a ataque/seguir e compara com as acoes manuais. Nao chama `Game::attack`, `Game::follow` ou outra funcao interna. Para `Map::getCreatureById` e `Map::getCreatureByName`, registra os bytes iniciais dos candidatos encontrados no perfil, mantendo `static_candidate_only` e `abi_validated: false`. Isso nao e uma validacao dessas funcoes em execucao.

O programa confere Timestamp, SizeOfImage, layout/hash da `.text`, caminhos direto/indireto e VTable do jogador. Ele le o ponteiro atual a cada amostra e nao usa `runtimeAddressDiagnostic` como endereco fixo. Se os dois caminhos estiverem disponiveis, precisam concordar. O caminho de processo com codificacao antiga no JSON nao e usado para abrir o alvo; o caminho atual vem do handle do processo selecionado.

O JSON de resultado fica na pasta atual com nome `otc_test_DATA_HORA.json`. Ele guarda cada leitura de capacidade separadamente. Um relatorio existente e recusado para evitar sobrescrita. As amostras nao sao atomicas: o processo continua executando.

## Validacao realizada

Quatro testes controlados passaram: duas capacidades, inclusive com valores diferentes; transicoes de ataque e seguir; rejeicao de fingerprint/VTable/caminho divergentes; preservacao do estado de candidato estatico das funcoes. A tentativa de leitura ao vivo nao encontrou o processo do Antica aberto; falta validar este monitor nele.

Somente consulta e leitura externa. Sem DLL, injecao, hooks, escrita ou execucao de funcoes internas.
