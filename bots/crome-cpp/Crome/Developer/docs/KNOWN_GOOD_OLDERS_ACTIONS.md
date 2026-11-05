# Olders – ações comprovadas nos testes anteriores

Profile: `6AA9FA57_89D3D35243890EA2`

| Ação | RVA | Evidência antiga | Estado no Crome v7.3 |
|---|---:|---|---|
| Walk | 0x1805C0 | bool=true + movimento observado | SDK CONFIRMED / UI bridge pending |
| Attack | 0x165410 | return normal + AttackTarget match | SDK CONFIRMED / UI bridge pending |
| Use | 0x164570 | return normal | SDK CONFIRMED / UI bridge pending |
| UseWith | 0x164A50 | F12, shovel, rope, fishing | SDK CONFIRMED / UI bridge pending |
| Talk | 0x165D40 | direct talk / healing path | SDK CONFIRMED / UI bridge pending |

O objetivo deste documento é impedir regressão: `nativeActionsReady=false` no profile não deve apagar evidência runtime já obtida para esta build.
