# Teste de um rato: ataque, corpo e moedas

Versao 5: este pacote executa um unico ciclo na build Antica ja conferida. A bag precisa estar aberta. Seleciona o **Rat vivo mais proximo no mesmo andar do jogador**, entre os objetos com posicao valida disponiveis no mapa do cliente. Nao exige que esteja adjacente e aceita varios candidatos, escolhendo o mais proximo; empates usam o ID. Comece sem ataque ou follow ativo. O personagem pode perseguir o rato usando o comportamento normal do cliente. Durante o teste, nao troque o alvo nem altere os containers.

Extraia todos os arquivos juntos. No PowerShell, entre na pasta extraida e rode:

```powershell
python -m pip install psutil pefile
python .\rat_cycle_once.py --pid 28440 --thread 26112 --destination 0
```

Use PID e thread atuais. A thread e a do ciclo do jogo confirmada no trace, que pode ser diferente da thread da janela. O programa confere isso novamente. Se o jogo reiniciou, os numeros antigos podem ter mudado.

## O que o teste faz

1. Confere a build, a bag 2853 e seleciona um Rat vivo com coordenadas validas no mesmo andar, pela proximidade. A mesma regra de andar e conferida no executavel nativo antes do ataque.
2. Solicita um ataque ao ID atual do rato, usando a referencia obtida pelo mapa do proprio cliente.
3. Acompanha as posicoes atuais do mesmo rato e do jogador por ate 60 segundos. A distancia pode aumentar durante a perseguicao. Registra corpos ja presentes nos tiles observados enquanto o rato esta vivo. Confirma em duas leituras que o alvo saiu do mapa: ID ausente/vazio, ou o mesmo objeto e controle com vida zero e a posicao invalida exata `65535,65535,255`. O segundo caso foi observado no seu relatorio: o objeto permanece na tabela depois de sair do mundo. Ainda exige um corpo novo 3994 na ultima posicao valida observada e o jogador adjacente antes de abrir. Essa e uma associacao por observacao do mapa, nao uma confirmacao por pacote de morte.
4. Reconfere o corpo na lista real de Things do Tile e solicita `Game::open` uma vez.
5. Exige uma nova janela de container com item 3994. Nao usa os antigos enderecos absolutos de heap como configuracao.
6. Recolhe todas as pilhas de moedas **3031**. Depois de cada movimento, confere a diminuicao no corpo e o aumento na bag. Outros itens permanecem no corpo.

Se a bag nao comportar uma pilha inteira, o movimento dessa pilha nao sera solicitado. O programa para em caso de mudanca de andar, corpo ambiguo, mudanca de identidade, falha de leitura ou transferencia nao confirmada. Tambem para se nao observou o ultimo tile antes da morte, em vez de abrir um corpo incerto. Nao repete uma chamada incerta. Sao permitidas ate oito transferencias confirmadas.

Uma chamada de ataque inicia o ataque normal do cliente; o programa nao envia ataques repetidos. Se o teste parar enquanto o rato ainda esta vivo, o ataque normal pode continuar no jogo. O programa nao solicita cancelamento automatico.

## Testar somente a abertura e o loot

Deixe **um unico corpo de rato adjacente ainda fechado**, com a bag aberta:

```powershell
python .\open_and_loot.py --pid 28440 --thread 26112 --destination 0
```

Para testar apenas a abertura, sem transferir moedas:

```powershell
python .\open_and_loot.py --pid 28440 --thread 26112 --destination 0 --open-only
```

## Relatorios

Na versao 5, `target_world_exit` registra como o alvo saiu do mapa e `target_world_exit_reads` conta as observacoes consecutivas. A vida zero com posicao invalida nao confirma loot por si so: sem corpo novo na ultima posicao valida, nenhuma abertura e solicitada. Posicao invalida com vida positiva, ID/objeto/controle diferentes ou falha de leitura continuam interrompendo o acompanhamento.

Na versao 4, `[ANDAR]` mostra o z do jogador e a quantidade de Rats vivos localizados nesse andar. O JSON guarda `player_at_selection` e `same_floor_candidates`. Coordenadas `65535,65535,255` continuam invalidas; nao representam um Rat localizado no andar atual. A leitura alcanca apenas criaturas disponiveis no mapa do cliente, nao descobre monstros de partes nao carregadas do mapa. Se nao houver candidato valido no andar, nao solicita ataque. O programa deixa a perseguicao a cargo do comportamento atual do cliente e nao envia uma rota de aproximacao separada.

Desde a versao 3, a selecao inicial descarta individualmente candidatos Rat com posicao/vida invalidas, sem impedir a escolha de outro Rat valido. `selection_diagnostics` registra ID, ponteiro, coordenadas, vida e bytes da posicao dos candidatos rejeitados; `rejected_candidates` informa o total. Se o proprio rato ja selecionado apresentar dados invalidos fora do estado terminal reconhecido na versao 5, o ciclo para e salva `read_error_details`.

Os JSONs sao salvos ao lado dos scripts e executaveis. O caminho completo aparece em `[RELATORIO]` ou `[JSON]`.

- `rat_cycle_*.json`: resultado do ciclo e relatorios de cada etapa.
- `open_loot_*.json`: teste isolado de abertura/coleta.
- `corpse_open_native_*.json`, `rat_attack_once_*.json`, `loot_native_*.json`: chamadas nativas.

`confirmed: true` no resultado final exige conferencia das moedas no corpo e na bag. Retorno da funcao, sozinho, nao confirma coleta. Corpo sem moedas resulta em coleta confirmada de zero moedas.

Ctrl+C pede parada apos a chamada atual retornar. Aguarde a restauracao; nao encerre o executavel nativo no meio de uma chamada.

## Validacao e limites

O ataque e a transferencia individual ja funcionaram no seu jogo. A abertura automatica e o ciclo completo deste pacote passaram em testes locais com processos controlados e memoria simulada; ainda precisam do teste no Antica.

Na versao 2, foi removida a exigencia de o personagem permanecer parado. O executavel de ataque tambem retorna ao controlador sem a espera de dois segundos do teste isolado, permitindo acompanhar a perseguicao mais cedo. Os testes simulam jogador e rato mudando de posicao e descartam um corpo antigo encontrado no caminho.

As leituras usam `ReadProcessMemory`. As acoes usam o depurador e a thread do jogo, com contexto e espaco temporario na pilha restaurados depois do retorno. Portanto esta etapa de acoes **nao e somente leitura**. Nao carrega DLL nem insere um bloco de codigo executavel. Nao execute outro depurador ao mesmo tempo.

O pacote aceita somente a build conhecida: timestamp `0x6AD91836`, SizeOfImage `0xEDE000`, hash FNV1a64 da .text `0xBF68517610291E3A`. Uma atualizacao diferente exige nova analise; este pacote nao descobre automaticamente todos os offsets de uma build nova.

Fontes C++ em `Source/`. Pontos usados nesta build: `Map::getCreatureById` RVA `0x20F0D0`, `Game::attack` `0x1434A0`, `Game::open` `0x142EE0`, `Game::move` `0x141E50`. A leitura dos tiles reproduz a consulta estatica de `Map::getTile`, sem chamar essa funcao.
