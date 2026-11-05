# Um passo externo

## Alternativa por mensagens da janela

Esta versao tambem permite testar `WM_KEYDOWN`/`WM_KEYUP` com `PostMessageW`:

Revisao 2: as setas recebem explicitamente o bit de tecla estendida mesmo quando `MapVirtualKeyW` retorna apenas `0x48`, sem prefixo E0. O log anterior `0x00480001`/`0xC0480001` mostrava a ausencia desse bit. Essa correcao nao garante que o cliente processe as mensagens.

```powershell
python .\walk_test.py --direction north --input postmessage
```

Depois do aviso, coloque o Antica em primeiro plano dentro de cinco segundos. O destino e a janela em primeiro plano cujo PID corresponde ao cliente selecionado. Nao depende de o titulo ser exatamente `Antica`. A soltura usa a mesma janela que recebeu a pressao.

O `lParam` inclui scan code, flag de tecla estendida e bits de soltura. O retorno de `PostMessageW` e verificado. Para seta para cima, os parametros esperados sao `0x01480001` ao pressionar e `0xC1480001` ao soltar. A tela mostra as duas mensagens e a posicao antes/depois.

`Mensagem enfileirada` confirma somente que o Windows aceitou colocar a mensagem na fila, nao que o jogo processou um comando de caminhada. Se nao houver mudanca de posicao, esse metodo tambem nao validou a caminhada. Nao instala hooks nem chama a funcao interna do OTC.

Oito testes controlados passaram, incluindo bits das mensagens, soltura na mesma janela e verificacao de falhas. Nenhuma mensagem real foi enviada ao Antica durante os testes.

Referencias: [PostMessageW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-postmessagew) e [campos da mensagem de teclado](https://learn.microsoft.com/en-us/windows/win32/inputdev/about-keyboard-input).

## Teste original por entrada do Windows

Este teste envia uma seta normal do Windows e compara a posicao antes/depois usando o perfil ja validado. Nao chama a funcao interna de caminhada do OTC e nao escreve na memoria do cliente.

Extraia todos os arquivos juntos em Cromin_py. Abra o cliente, entre no personagem e feche o chat. Fique em local livre, sem ataque/seguir ou caminhada automatica ativos.

```powershell
Set-Location "C:\Users\Usuario\OneDrive\Documentos\Cromin_py"
python .\walk_test.py --direction north
```

Depois da verificacao da build, o programa avisa que voce tem cinco segundos para colocar a janela do jogo em primeiro plano. Clique no jogo, solte as teclas e aguarde. Se outra aplicacao estiver em primeiro plano quando acabar o prazo, ele cancela a seta.

Direcoes: `north` (cima), `south` (baixo), `east` (direita), `west` (esquerda).

```powershell
python .\walk_test.py --direction east
```

Cada execucao faz uma unica pressao de tecla, seguida da soltura. Nao tenta novamente se a posicao nao mudar. A entrada e global do Windows; mantenha o foco no jogo durante o teste. O programa nao tenta detectar o chat interno: feche-o antes de executar. Confira o resultado de posicao no terminal quando terminar.

Para verificar apenas a leitura, sem enviar tecla:

```powershell
python .\walk_test.py --dry-run
```

Se o jogo estiver elevado, o Windows pode impedir que um programa com menos privilegios envie entrada. [Documentacao de SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput).

Cinco testes controlados passaram: tamanho da estrutura Windows x64, uma pressao/soltura, cancelamento por foco diferente, cancelamento por modificador pressionado e soltura em interrupcao. Nenhuma seta real foi enviada durante esses testes; a resposta do Antica ainda precisa ser testada.
