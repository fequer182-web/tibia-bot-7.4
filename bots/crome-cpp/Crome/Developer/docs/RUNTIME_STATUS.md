# Runtime status v6.3

## Implementado e conectado
- Detecção do processo a partir dos executáveis descritos nos BuildProfile v2.
- Validação de PE por timestamp e SizeOfImage antes de aceitar o profile.
- OpenProcess / ReadProcessMemory.
- Resolução de LocalPlayer a partir do profile.
- Validação opcional da vtable do LocalPlayer.
- PlayerReader real: posição, direção, HP, MaxHP, Mana, MaxMana, capacity, experience, level e demais campos confirmados presentes no profile.
- UI mostra snapshot real no rodapé.
- Cavebot `Record` usa a posição real do LocalPlayer.
- UI corrigida: Healing/UH e Tools/Food possuem altura suficiente e não ficam cortados.

## Bloqueio intencional de ações nativas
Os profiles fornecidos marcam `nativeActionsReady=false`. No profile completo do Olders, os endereços de ações existem, mas o próprio profile marca `callable=false`, `machineAbiValidated=false` e `threadValidated=false` para as rotinas nativas. Por isso esta versão NÃO força chamadas C++ nativas não validadas: isso poderia derrubar o cliente.

A arquitetura de Healing, Support, RuneMaker, Targeting, Cavebot e Looter continua pronta em `modules/`; assim que o BuildMigrator promover uma ação para callable/ABI+thread validados, o backend pode ser habilitado sem mudar a UI ou regras do bot.
