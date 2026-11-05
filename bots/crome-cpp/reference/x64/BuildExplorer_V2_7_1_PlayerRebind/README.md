# OTCLab BuildExplorer V2.7 — x64 Look AUTO + Player Light

O export agora inclui `pipeDiscovery`: catálogo de métodos buscados na build atual, sem habilitar chamadas. Veja [padrão de descoberta do pipe x64](../PIPE_DISCOVERY.md). Use o JSON recém-gerado no ABIBuilder atualizado.

Esta versao parte da V2.6/V4.9.1 que ja validou Look + Light no Classico e preserva o core V4.9 DIRECT BIND. A mudanca principal e que **builds x64 novas nao precisam mais de um probe separado apenas para descobrir o ABI do Look**.

## Compilar

Execute `build.bat` no Windows.

Compilador esperado:

`C:\msys64\ucrt64\bin\g++.exe`

Saida:

`Build\OTCLab_BuildExplorer_V2_7_X64_LOOK_AUTO.dll`

## Look AUTO em build nova

Quando o resolver estatico encontrar `Game::look` ou `Game::detailedLook`, mas ainda nao souber o ABI runtime, a V2.7 oferece `LOOK AUTO DISCOVERY`.

O teste:

1. usa um breakpoint temporario de **1 byte (INT3)** na entrada da funcao candidata;
2. captura `RCX`, `RDX`, `R8`, `R9` no momento real do Look;
3. testa cada registrador diretamente e com uma indirecao `*(REG)`;
4. valida o objeto contra a VTable de `Item`, quando ela ja estiver confirmada;
5. le o ID usando o `itemIdOffset` que a propria build ja descobriu;
6. exige duas amostras de IDs diferentes confirmando **o mesmo registrador + mesma indirecao + mesmo offset**;
7. restaura o byte original antes de sair.

IDs sugeridos pelos testes atuais:

- Item A: `3031`
- Item B: `3552`

Esses IDs sao apenas defaults. **Nao sao tratados como universais entre OTs**. A DLL pergunta se deseja usa-los e permite informar outros IDs conhecidos.

Se o Look nao fechar, a DLL tenta ate **3 ciclos**. Depois o orquestrador existente exibe:

- `R` = repetir/calibrar novamente;
- `S` = pular e salvar como pending;
- `Q` = salvar o que ja foi encontrado e encerrar o wizard.

Quando fecha, salva no BuildProfile:

- `lookFeatureStatus = confirmed_runtime`
- `lookHookFunction`
- `lookHookRva`
- `lookHookArgument`
- `lookThingIndirection`
- `lookItemIdOffset`

## Builds de referencia

### Classico

Continua usando o contrato ja comprovado:

- `Game::look @ 0x164090`
- `Thing* = *(RDX)`
- Item ID `+0x40`
- Player Light `LocalPlayer +0xF1`, `u8`, hack value `15`, com restore.

### Olders OpenGL

Continua usando o contrato ja comprovado:

- `Game::detailedLook @ 0x163B10`
- `Thing* = *(RDX)`
- Item ID `+0x38`
- X/Y/Z `+0x18/+0x1C/+0x20`
- `x == 65535` indica container/inventory.

### Olders DX9 atual

O profile parcial `6AA9FB5B / 9F444FE4962780AE` foi incluido para facilitar o reteste. Ele ja possui core/Item/Functions descobertos e deixa o Look para a nova calibracao AUTO.

## Player Light

A rotina de descoberta continua baseada em transicoes reais:

- `BASE -> TOCHA -> BASE`
- e/ou `BASE -> utevo lux -> BASE`

`utevo gran lux` pode ser usado como evidencia adicional. Um offset so vira `confirmed_runtime_write` depois do teste ativo de escrita + restore e confirmacao visual.

## Importante

- O discovery principal continua READ-ONLY.
- O Look AUTO so altera temporariamente um byte na funcao candidata e restaura ao sair.
- Light sempre salva/restaura o valor original no teste ativo.
- Nao injete probes antigos de Look/Light simultaneamente.
- Esta fonte nao foi compilada neste ambiente Linux; compile com o MSYS2 x64 do seu Windows e envie qualquer erro exato se aparecer.
