# x64 Look + Player Light — contrato V2.7

## Look em builds conhecidas

Classico e Olders OpenGL mantem os hooks/prologos ja comprovados pelos testes anteriores.

## Look AUTO em build nova

O resolver estatico fornece um candidato `Game::look`/`Game::detailedLook` e o layout de Item fornece `itemIdOffset`. A V2.7 fecha o restante do contrato ao vivo.

### Captura

Um breakpoint `INT3` temporario de um byte e colocado na entrada da funcao candidata. O Vectored Exception Handler captura:

- RCX
- RDX
- R8
- R9

Para cada registrador testa:

- `REG` como `Thing*`
- `*(REG)` como `Thing*`

Quando `itemVtableRva` esta confirmado, o candidato precisa possuir a mesma VTable antes de o ID ser aceito.

### Dupla prova

Amostra A e Amostra B devem ter IDs conhecidos e diferentes. O mesmo caminho precisa produzir ambos os IDs em eventos separados.

Exemplo de resultado valido:

- A: `*(RDX) + itemIdOffset = 3031`
- B: `*(RDX) + itemIdOffset = 3552`

Resultado salvo:

- `status = confirmed_runtime`
- `hookArgument = RDX`
- `thingIndirection = 1`

Os valores `3031` e `3552` sao defaults de conveniencia e podem ser trocados antes da calibracao.

### Retry

Sao executados ate 3 ciclos. Sem evidencia suficiente, o orquestrador do wizard oferece R/S/Q.

## Player Light

A descoberta continua por transicao real do LocalPlayer:

- BASE -> TOCHA -> BASE; ou
- BASE -> utevo lux -> BASE.

O teste de escrita faz snapshot do byte original, forca o valor de hack temporariamente e restaura o original. `confirmed_runtime_write` so e emitido depois da confirmacao do write + restore.
