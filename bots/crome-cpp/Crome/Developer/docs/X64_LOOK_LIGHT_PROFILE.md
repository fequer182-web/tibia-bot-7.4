# Crome x64 Feature Profile — Look ID + HackLight

## Objetivo
Este documento define o contrato x64 usado pelo Crome para duas capacidades que nao devem ficar hardcoded nos modulos do bot:

- **Look ID**: resolve o objeto observado pelo jogador e expoe o ID do item, alem de contexto ground/container quando a build suporta.
- **HackLight**: forca o campo confirmado de intensidade de luz do LocalPlayer, salvando e restaurando o valor original.

A fonte de verdade e cada `OTCLab_BuildProfile_*.json`. O bot deve consumir `features.look` e `features.hackLight`; offsets e RVAs permanecem no profile da build.

## Convencao x64 observada
Nos clientes OTC-derived x64 testados, os primeiros argumentos inteiros/ponteiros aparecem em `RCX`, `RDX`, `R8` e `R9`. Isto descreve a ABI observada; a semantica de cada funcao continua exigindo validacao por build.

Um profile pode descrever `hookArgument: "RDX"` e `thingIndirection: 1`, significando que o objeto confirmado e `Thing* = *(RDX)`.

## Olders OpenGL — 6AA9FA57 / 89D3D35243890EA2

### Look ID confirmado
- Funcao: `Game::detailedLook`
- RVA: `0x163B10`
- Thing: `*(RDX)`
- Item ID: `Thing + 0x38` (`u32`)
- Posicao ground: `Thing + 0x18/+0x1C/+0x20`
- Sentinel container/inventory observado: `X == 65535`

Classificacao:
- `x != 65535` -> ground; X/Y/Z validos.
- `x == 65535` -> container/inventory; nao exibir como posicao de mapa.

### HackLight
Permanece `pending`. O ambiente Olders usado nos testes nao ofereceu transicao escuro/luz util, portanto nenhum offset foi inventado.

## Classico — 6AA3019A / 602E746BAE706D6C

### Look ID confirmado
- Funcao: `Game::look`
- RVA: `0x164090`
- Thing: `*(RDX)`
- Item ID: `Thing + 0x40` (`u32`)

Nao reutilizar `LocalPlayer/Creature +0x30/+0x34/+0x38` como coordenadas de Item. Nos testes de Item esses campos produziram `0x3F800000`/`1.0f`; por isso posicao/contexto de Item continuam `pending` nessa build.

### HackLight confirmado
- Storage: campo do LocalPlayer
- Offset: `+0xF1`
- Tipo: `u8`
- Sem luz observado: `0`
- `utevo lux`: `6`
- `utevo gran lux`: `8`
- Valor de HackLight testado: `15`
- Restore: salvar byte original antes de forcar e restaurar ao desligar/descarregar.

Transicao validada: `0 -> 6 -> 8 -> 0`.

O WriteProbe forçou `6`, `8` e `15` visualmente e restaurou o valor original.

## Contrato JSON
```json
"features": {
  "look": {
    "status": "confirmed_runtime",
    "architecture": "x64",
    "hookFunction": "gameLook",
    "hookRva": "0x164090",
    "hookArgument": "RDX",
    "thingIndirection": 1,
    "itemIdOffset": "0x40",
    "itemIdType": "u32"
  },
  "hackLight": {
    "status": "confirmed_runtime_write",
    "architecture": "x64",
    "storage": "local_player_field",
    "intensityOffset": "0xF1",
    "type": "u8",
    "hackValue": 15,
    "restorePolicy": "snapshot_original"
  }
}
```

## Regras de consumo pelo Crome
1. Selecionar primeiro o profile pelo executavel/fingerprint.
2. Habilitar Look ID apenas quando `features.look.status` comecar por `confirmed`.
3. Habilitar HackLight apenas quando `features.hackLight.status` comecar por `confirmed`, o tipo for suportado e o LocalPlayer passar a validacao do profile.
4. Nunca copiar offsets entre builds: os dois x64 testados ja diferem em Item ID (`+0x38` vs `+0x40`), funcao de Look e layouts.
5. HackLight deve guardar o byte original antes de forcar e restaurar em OFF/END/unload.
6. `candidate` e `pending` nunca sao tratados como confirmados pelo runtime de release.

## Testes mantidos no projeto
- `Developer/tests/x64_features/look_id_classico/`: valida `Game::look`, `*(RDX)` e `Item +0x40`.
- `Developer/tests/x64_features/player_light_classico/`: valida `LocalPlayer +0xF1` por escrita controlada e restore.

Use `Developer/build_x64_feature_tests.bat`, ou `Developer/build.bat tests` para compilar o Crome e depois os dois calibradores.
