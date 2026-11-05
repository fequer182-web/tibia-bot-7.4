# Caminhar ate um SQM

Execute no PowerShell da pasta extraida, com o jogo aberto na mesma sessao:

```powershell
.\OTC_Walk_To.exe --pid 28440 --thread 26112 --x 32092 --y 32197 --z 7
```

Usa Game::walk da build ja validada, uma chamada por passo. Primeiro ajusta X e depois Y. Nao usa teclado. Nao pressione setas, clique no mapa ou ative seguir/autoWalk durante o trajeto. Confere a posicao apos cada chamada; se nao houver exatamente o movimento esperado, para sem repetir. Nao tem mapa de colisao nem desvio de obstaculos. Outro andar ou distancia acima de 100 SQMs cancela antes das chamadas.

Mostra [CHEGOU] somente ao ler as coordenadas do destino. Salva walk_to_NUMERO_step_N.json ao lado do executavel para cada chamada. Ctrl+C solicita parada entre chamadas; aguarde a chamada atual retornar. Nao feche a janela durante [CHAMADA]. Usa as mesmas alteracoes temporarias de contexto/pilha descritas em LEIA-ME.md.

Se a sessao reiniciou, refaca a captura e use PID/thread atuais. O programa confere fingerprint, checkpoint e thread em memoria antes do trajeto. O ambiente do agente nao enxerga a sessao do jogo: o trajeto real depende da execucao local pelo usuario.

Compilar com MSYS2 UCRT64:

```powershell
& 'C:\msys64\ucrt64\bin\g++.exe' -std=c++17 -O2 -Wall -Wextra -static -s .\Source\walk_to.cpp -o .\OTC_Walk_To.exe
```

O --self-test verifica decisoes de direcao, chegada, recusa de outro andar, bloqueio/desvio e uma rota simulada de 20 passos. A chamada nativa reutiliza o mecanismo de chamada unica previamente testado; a caminhada completa ainda nao foi executada no Antica pelo agente.
