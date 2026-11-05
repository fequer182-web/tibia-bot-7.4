# Cromin UI - primeira versao dentro do Antica

A DLL cria um painel filho dentro da janela do Antica. As abas sao Ataque, Loot, Cavebot e Healing. Um controlador Python permanece aberto para executar as chamadas nativas ja usadas nos testes anteriores. Todas as automacoes iniciam desligadas, inclusive depois de reabrir o programa.

## Iniciar

Extraia todos os arquivos do ZIP juntos em uma pasta nova, sem misturar com as versoes anteriores dos testes. Com o Antica aberto e o personagem conectado, abra o PowerShell nessa pasta:

```powershell
python -m pip install -r .\requirements.txt
python .\iniciar_ui.py --pid 28440 --thread 26112
```

PID e thread acima sao os da sessao atual. Tambem pode deixar o programa localizar o processo e ler a thread do jogo:

```powershell
python .\iniciar_ui.py
```

Mantenha o PowerShell aberto. Primeiro confira se o painel apareceu dentro do Antica com todas as caixas desligadas. Depois ative os modulos desejados. A UI fica junto da borda direita e tem rolagem se a janela for baixa. Para visualizar apenas o painel em uma janela de teste, sem carregar nada no jogo, execute `Cromin_UI_Preview.exe`.

## Controles desta versao

| Aba | Comportamento |
| --- | --- |
| Ataque | Auto ataque continuo a Rats vivos no mesmo andar. Escolhe o mais proximo e acompanha o mesmo ID. Nao troca um alvo manual ja ativo. |
| Loot | Moedas 3031 para a bag 2853 aberta. Com Ataque, acompanha o corpo novo do alvo e confere os totais. Sozinho, procura um unico corpo de rato adjacente ainda nao tratado nesta sessao. |
| Cavebot | Grave waypoints pelo botao Adicionar posicao atual. Usa o autoWalk nativo e so avanca apos observar a chegada. Repetir rota e opcional. |
| Healing | Informe a fala da magia, um limite de HP absoluto e o intervalo minimo. Solicita a magia enquanto HP estiver abaixo ou igual ao limite. |

Abra a bag antes de ativar o loot e configure o numero da janela dela (normalmente 0). O primeiro teste recomendado e Ataque + Loot, que reaproveita o ciclo confirmado no jogo. Essa combinacao continua procurando outro Rat depois de terminar um ciclo.

Para gravar uma rota, deixe o Cavebot desligado, va manualmente a cada ponto e clique Adicionar posicao atual. Ative quando terminar. Esta versao cobre caminhada no mesmo andar, com ate 64 pontos; cada trecho deve ficar ate 100 SQMs de distancia Manhattan. Escadas, buracos, ferramentas e trocas de andar ainda nao fazem parte desta rota. Se nao observar chegada em 60 segundos, para sem repetir a mesma chamada incerta.

O healing usa o HP atual em `LocalPlayer+0x6C0`, que foi identificado anteriormente. Nao usa porcentagem porque o HP maximo ainda nao foi confirmado. A fala aceita ate 15 caracteres ASCII e inicia vazia: escolha a magia que seu personagem pode usar. Esta versao nao usa pocao. O retorno de Game::talk confirma a chamada, mas nao confirma que o servidor aceitou a magia.

As acoes sao serializadas. O healing e verificado entre chamadas e durante o acompanhamento do combate e da caminhada; nao interrompe uma chamada nativa em andamento. Portanto nao ha garantia de latencia fixa de cura.

## Parar e fechar

PARAR ou PARAR AUTOMACOES desliga novas solicitacoes. A chamada atual pode terminar, e um ataque ou uma rota ja iniciados no cliente podem continuar. Interrompa essas acoes no proprio jogo se necessario.

Fechar o painel desliga as automacoes, encerra a janela e permite descarregar a DLL. O controlador encerra quando percebe o fechamento. Feche o painel antes de iniciar outro controlador para o mesmo PID. Ctrl+C no PowerShell aguarda a operacao atual e encerra o controlador; se o painel permanecer aberto, feche-o no jogo.

Configuracoes e waypoints ficam em `Cromin_UI.settings.json`. Os estados ligado/desligado nao sao restaurados automaticamente. Relatorios ficam nesta pasta: `ui_session_*.jsonl` e os JSONs individuais das chamadas. Em falha nao confirmada, o controlador desliga as automacoes; feche o painel e reinicie para um novo teste.

## O que foi validado

- Ataque, perseguicao, abertura do corpo e transferencia de moedas ja foram confirmados no seu Antica com o Rat Cycle 5.
- A DLL nova foi carregada em um processo de teste local: criou o painel filho, transmitiu os controles por memoria compartilhada, rolou em janela baixa e descarregou ao fechar.
- Testes do controlador verificaram limite/cooldown do healing, prioridade sobre waypoints, envio unico da rota e bloqueio de chamadas concorrentes.
- A interface injetada, os modulos continuos, o cavebot e o healing ainda precisam de validacao dentro do Antica. O ciclo anterior validado nao confirma automaticamente toda essa integracao.

O carregador confere a build conhecida antes de usar LoadLibraryW: timestamp `0x6AD91836`, SizeOfImage `0xEDE000` e FNV1a64 da .text `0xBF68517610291E3A`. Nao altera protecoes do cliente. Esta etapa carrega DLL e executa acoes, portanto nao e somente leitura.

O [vBot](https://github.com/Vithrax/vBot) serviu de referencia para a organizacao por modulos. Seus arquivos dependem do [ambiente Lua game_bot](https://github.com/OTCv8/otclientv8/blob/master/modules/game_bot/executor.lua). Esta primeira interface foi recriada com controles Windows; ela nao instala o vBot nem reproduz todo o seu visual ou suas funcoes.

Fontes em `Source/`, testes em `Tests/`. Execute `Tests/test_controller.py` para os testes simulados. O teste nativo da DLL e `Cromin_UI_Dll_Test.exe`; ele usa sua propria janela e nao se conecta ao Antica.
