# Arquitetura definitiva

1. **BuildMigrator produz; Crome consome.** O contrato é `OTCLab_BuildProfile_v2`.
2. **Offsets/RVAs não sobem.** Módulos de bot não conhecem endereços.
3. **Profiles parciais são válidos.** `Capabilities` ativa apenas o que o profile suporta.
4. **Fingerprint seleciona a build.** `ProfileRegistry` casa timestamp + imageSize + textHash.
5. **Readers convertem memória em snapshots.** `ProfileBackedPlayerReader` já lê tipos definidos pelo JSON.
6. **Actions são API estável.** Backend interno futuro implementa `IGameActions` com dispatcher da game thread.
7. **Uma leitura de estado por tick.** `GameStateService` distribui o mesmo snapshot a todos os módulos.
8. **Módulos não brigam.** `ActionScheduler` recebe ações com prioridade e executa uma por ciclo.
9. **Config de servidor não é build.** IDs de food/rope/shovel/pathable ficam em `config/items.json`.
10. **UI não executa game.** A UI edita configs e mostra diagnóstico/capabilities.
