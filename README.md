# Tibia Bot 7.4

Painel compacto para Windows com leitura de personagem, cavebot por setas, ataque por hotkey e teste de abertura de corpos. A versão principal é para **AncientiaDX x86**. O código anterior de Antica está em `legado/antica`.

O [catálogo de bots](docs/BOTS.md) reúne as outras versões e os laboratórios. O [guia de localização](docs/LOCALIZACAO.md) indica como encontrar o PID, reconhecer a build, buscar valores e testar um perfil ou pipe.

## Instalação do compacto Ancientia

1. Baixe ou clone o repositório e instale Python para Windows com Tkinter.
2. Abra o Ancientia pelo launcher e entre no personagem.
3. No PowerShell, dentro do repositório, execute:

```powershell
python -m pip install -r requirements.txt
Get-Process AncientiaDX | Select-Object Id, MainWindowTitle
python iniciar.py
```

No compacto, abra **Conectar**, informe o PID do DX e conecte. Se o Windows negar o acesso, execute o terminal com o mesmo nível de permissão do cliente. A conexão verifica a build pelo hash do executável; após atualização do jogo é necessário validar um novo perfil.

## Como usar

| Botão | O que faz | Estado |
| --- | --- | --- |
| Cavebot | Grava posições e percorre pontos próximos no mesmo andar com as setas. | Sem busca de caminho ou mudança de andar. |
| Targeting | Usa a hotkey configurada no cliente e confere o alvo pela leitura. | Confira a tecla manualmente antes de ativar. |
| Loot | Clica com o botão direito nos SQMs adjacentes e verifica se abriu um recipiente. | Experimental. Leitura da bag e do corpo confirmada; clique automático ainda pendente de validação no jogo. |
| Luz | Testa intensidade de luz com escrita de um byte e tentativa de restauração ao desligar. | Efeito visual pendente de confirmação. |
| Healing | Reservado para uma adaptação futura. | Indisponível. |

**Ligar todos** ativa Cavebot, Targeting, Loot e Luz juntos. Grave um waypoint, configure o Targeting, calibre o mapa e abra a bag 0 antes de usar. A caminhada aguarda o combate e a tentativa de loot para que as ações usem o teclado e mouse em ordem.

**F8 ou Parar** desliga as automações. O cliente precisa estar em primeiro plano para receber teclas ou cliques. Fechar a janela de configuração só a oculta; encerre pelo compacto para desconectar.

### Loot

Abra a bag 0 e feche corpos antigos. Em **Loot**, calibre o centro do SQM do personagem e o centro do SQM à direita. Cada captura dá cinco segundos para voltar ao jogo. Recalibre se mudar zoom ou painéis.

Para testar sem depender de combate, fique parado perto de um corpo e use **Varrer os 8 SQMs atuais em 5 segundos**. O modo automático começa pela última posição adjacente do alvo e pode tentar as outras casas ao redor. Desaparecer não comprova morte: cancelar um ataque também pode iniciar uma tentativa. Novo alvo, movimento, abertura de recipiente ou limite de tempo encerram a varredura. O bot não abre corpos distantes ou em outro andar e não arrasta itens; quem recolhe é o auto loot do jogo. Um recipiente aberto não comprova transferência de itens.

## Organização

- `iniciar.py` e `iniciar.cmd`: entrada do compacto.
- `src/cromin/`: leitores da build, ações de teclado/mouse e controle da rota, combate e loot.
- `ancientia_loot.py`: inspeção de recipientes abertos sem transferir itens (`--pid` obrigatório).
- `tests/`: testes locais de leitura, entrada e automação simulada.
- `pesquisa/`: BuildProbe e experimentos de diagnóstico; não fazem parte do fluxo normal.
- `legado/antica/`: código e documentação históricos da versão Antica. Endereços e chamadas nativas desse cliente não se aplicam ao Ancientia.

A versão principal não carrega DLL, não usa Lua e não modifica `Crome.exe`. Consulte [como funciona](docs/ARQUITETURA.md) para a sequência de validação. Não há cliente de jogo, executáveis distribuídos, relatórios reais ou configurações pessoais no repositório.

## Testes locais

```powershell
python -m unittest discover -s tests
python iniciar.py --self-test
```

Os testes simulam memória e entradas; não confirmam o funcionamento do loot no servidor.
