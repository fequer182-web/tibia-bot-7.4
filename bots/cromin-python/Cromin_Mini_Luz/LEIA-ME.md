# Cromin Mini Luz

DLL x64 de 40 KB, com somente o experimento de iluminacao do personagem. Inicia desligada. Nao tem painel, Python, comunicacao compartilhada ou modulos de ataque, loot, caminhada e healing.

O campo `LocalPlayer+0xF1` foi identificado por mudancas de leitura (0 -> 7), mas seu efeito visual quando escrito ainda nao foi validado no Antica. Esta DLL altera apenas esse byte candidato, com valores entre 0 e 31. Ela nao altera o byte de cor nem a iluminacao global do mapa.

## Carregar

Extraia o ZIP. Com o Antica aberto e o personagem conectado, entre na pasta `Cromin_Mini_Luz` no PowerShell:

```powershell
.\Mini_Luz_Load.exe --pid 28440 .\Cromin_Luz.dll
```

Use o PID atual. O carregador confere a build antes de chamar LoadLibraryW. A DLL tambem confere o PE e o hash da .text antes de permitir qualquer escrita.

Tambem pode usar o mesmo carregador normal que utilizou com sua DLL anterior, selecionando `Cromin_Luz.dll`. O carregador da UI anterior nao serve para esta DLL porque exigia a comunicacao do painel.

## Teclas, com o jogo em foco

| Tecla | Acao |
| --- | --- |
| F6 | Ativar/desativar; primeiro valor selecionado e 7 |
| F7 | Diminuir o valor selecionado |
| F8 | Aumentar o valor selecionado, ate 31 |
| F9 | Tentar restaurar o valor anterior e descarregar a DLL |

As teclas nao sao consumidas: se voce ja usa essas teclas como hotkeys do jogo, elas tambem podem acionar essas hotkeys. Enquanto ligada, a DLL verifica o jogador e mantem o valor selecionado a cada 100 ms. Se o ponteiro do jogador mudar ou os dados deixarem de conferir, desativa a escrita; nao segue um endereco antigo de heap.

Ao desativar, tenta restaurar o valor anterior somente se o mesmo jogador ainda estiver valido e o byte ainda corresponder ao ultimo valor escrito. Se o cliente atualizou o byte, preserva essa atualizacao. Uma falha de restauracao fica no log. Use F9 para descarregar; nao force a remocao da DLL enquanto sua rotina estiver executando.

## Resultado do teste

`Cromin_Luz.log` fica ao lado da DLL. Procure `BUILD_OK`, `ENABLE_CANDIDATE`, `LIGHT_BYTE_WRITTEN`, `RESTORED` ou a falha registrada. `LIGHT_BYTE_WRITTEN` confirma apenas que o byte foi escrito e relido; confira visualmente se mudou a iluminacao.

Os testes locais usam memoria sintetica propria e passaram para validacao, escrita de um byte, restauracao, troca do jogador e rejeicao de pagina somente leitura. Para repetir:

```powershell
.\Mini_Luz_Test.exe
```

Esta DLL usa inicializacao normal: DllMain, DisableThreadLibraryCalls e CreateThread. Nao oculta o modulo nem altera o anti-cheat. Ser pequena nao garante que o cliente a aceite; a aceitacao pelo anti-cheat e o efeito da escrita no jogo continuam sem validacao.

Build aceita: timestamp `0x6AD91836`, SizeOfImage `0xEDE000`, .text FNV1a64 `0xBF68517610291E3A`. Fontes em `Source/`. Se a build divergir, o carregador recusa e a DLL nao permite escrita.
