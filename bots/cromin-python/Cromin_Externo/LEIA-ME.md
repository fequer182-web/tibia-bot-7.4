# Cromin - painel externo

Esta versao abre uma janela propria do Cromin_py. Nao carrega a DLL do painel nem a DLL de luz no Antica. Usa leitura externa e os executaveis de chamadas nativas dos testes anteriores.

## Primeiro teste: ataque e loot

1. Extraia o ZIP em uma pasta nova. Encerre os programas de teste anteriores. Se ainda houver uma DLL antiga carregada, reabra o cliente para testar apenas esta versao.
2. Com o personagem conectado e a bag aberta, entre na pasta `Cromin_Externo` pelo PowerShell e execute:

```powershell
python -m pip install -r .\requirements.txt
python .\painel_externo.py
```

3. Clique **Conectar ao Antica**. Aguarde a conferencia da build e veja o HP e as coordenadas no painel. Todas as automacoes iniciam desligadas.
4. Na aba Loot, confira a bag de destino (normalmente janela 0) e ative o auto loot. Na aba Ataque, ative o auto ataque. Comece sem outro ataque/follow ativo e com um Rat no mesmo andar.
5. Confira a morte, abertura do corpo e moedas recebidas. Para encerrar o teste, clique **PARAR AUTOMACOES**.

Se houver mais de um cliente aberto, escolha o PID explicitamente:

```powershell
python .\painel_externo.py --pid 28440 --thread 26112
```

Os numeros acima pertencem a sessao anterior e podem mudar. Sem argumentos, o programa localiza o processo e le a thread do ciclo do jogo. Nao inicia nenhuma acao ao abrir ou conectar.

## O que cada aba faz

| Aba | Escopo atual |
| --- | --- |
| Ataque | Procura Rats vivos no mesmo andar, escolhe o mais proximo e acompanha o mesmo ID. Com o modulo ligado, procura outro rato depois do ciclo. |
| Loot | Recolhe moedas 3031 para a bag 2853 aberta. Confere as quantidades no corpo e no destino. Com ataque desligado, pode tratar um unico corpo de rato adjacente ainda nao processado nesta sessao. |
| Cavebot | Grave pontos com Adicionar posicao enquanto estiver parado no local e com o cavebot desligado. Ate 64 pontos no mesmo andar; cada trecho ate 100 SQMs. Avanca apos observar chegada, com repeticao opcional. |
| Healing | Fala de magia configuravel, limite de HP absoluto e intervalo minimo. Nao usa pocao nem porcentagem de HP. |
| Luz | Experimental: ajusta somente o byte candidato de intensidade do personagem, de 0 a 31. Inicia desligada; valor inicial 7. |

O healing e verificado entre chamadas e durante o acompanhamento do combate/caminhada. Nao interrompe uma chamada nativa em andamento, portanto nao ha garantia de latencia fixa. O retorno de Game::talk confirma a chamada; nao confirma que o servidor aceitou a magia.

Os relatorios enviados confirmaram o painel externo em ciclos continuos de ataque e loot, com tres transferencias de 2, 1 e 3 moedas (bag: 25 para 31). Cavebot e healing combinados ainda precisam de validacao no Antica.

## Testar a luz

1. Conecte o painel, deixando ataque, loot, cavebot e healing desligados durante este teste.
2. Abra a aba Luz. Comece com intensidade 7, clique Aplicar configuracao e marque Ativar luz do personagem.
3. Confira visualmente em uma area escura. A mensagem Valor aplicado confirma a leitura do byte escrito; nao confirma o efeito visual.
4. Desmarque a luz para testar a restauracao. PARAR AUTOMACOES, desconectar e fechar tambem solicitam a restauracao.

A luz usa uma escrita externa de um byte em LocalPlayer + 0xF1, apos conferir jogador, VTable, posicao e faixa do valor. O acesso de escrita so e aberto quando necessario. Nao altera cor nem iluminacao global. O efeito visual deste campo ainda nao foi confirmado nesta versao. Se o jogador mudou ou ficou indisponivel, evita escrever no objeto antigo; se o cliente atualizou o valor, preserva essa atualizacao. Consulte light_enabled, light_byte_written e light_disabled no relatorio da sessao.

## Alvo fora do mapa carregado

A versao 3 ignora apenas o ciclo atual quando o ultimo tile nao foi observado antes da morte, o corpo desapareceu, ficou fora de alcance ou o tile mudou durante a leitura. Registra cycle_skipped, mantem a conexao e os modulos ligados e continua a busca, sem abrir corpo incerto. Falhas de chamada nativa, restauracao ou estrutura de memoria continuam interrompendo as automacoes. Esta correcao passou em simulacao do erro mostrado no painel; falta validacao ao vivo.

A versao 2 reconhece o estado observado nos seus relatorios: mesmo Rat, mesma identidade, vida positiva e posicao 65535,65535,255. Registra target_lost e encerra esse acompanhamento sem confirmar morte, abrir corpo ou coletar moedas. O painel continua ativo, aguardando o ataque/follow atual terminar antes de procurar outro alvo. Falhas de identidade ou restauracao continuam interrompendo as automacoes.

## Parada, fechamento e relatorios

PARAR AUTOMACOES impede novas solicitacoes. Ataque ou rota ja enviados podem continuar no cliente; interrompa-os no proprio jogo se necessario. Desconectar e fechar aguardam a chamada atual retornar e restaurar o contexto antes de encerrar. Nao force o encerramento dos executaveis durante uma chamada.

Uma falha de chamada/restauracao interrompe as automacoes. Veja a mensagem no painel e desconecte/reconecte para outro teste. As configuracoes e waypoints ficam em `Cromin_Externo.settings.json`, mas as automacoes sempre iniciam desligadas. Os relatorios ficam ao lado dos scripts: `external_session_*.jsonl` e os JSONs individuais de ataque, abertura, loot, caminhada e fala.

Para abrir apenas uma previa, sem conectar ao jogo:

```powershell
python .\painel_externo.py --preview
```

## Limites tecnicos

O painel nao injeta DLL. As acoes continuam usando o depurador para executar uma chamada na thread do jogo e restaurar contexto/pilha, como nos testes anteriores. Portanto nao sao somente leitura, e o painel externo nao garante aceitacao pelo anti-cheat.

Somente a build conhecida e aceita: timestamp `0x6AD91836`, SizeOfImage `0xEDE000`, .text FNV1a64 `0xBF68517610291E3A`. Atualizacoes diferentes exigem nova analise.

Testes locais passaram para as cinco abas, estado inicial desligado, conexao simulada em segundo plano, controle unico por PID, HP/cooldown, prioridade do healing, waypoint enviado uma vez e bloqueio de chamadas concorrentes. A perda de alvo foi reproduzida em teste simulado; escrita de luz/restauracao foram testadas em memoria simulada e em um processo de teste proprio. A luz e a correcao de perda de alvo ainda precisam de teste ao vivo no Antica.
