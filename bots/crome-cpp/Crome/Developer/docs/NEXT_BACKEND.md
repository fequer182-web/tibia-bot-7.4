# Ponto de encaixe do receptor/injetor

A arquitetura está pronta para o backend específico do processo. O receptor precisa fornecer:

- `RuntimeContext.moduleBase`, `localPlayer`, `game`, `map`;
- uma implementação de `IMemory`;
- implementações de `ICreatureReader`, `IContainerReader`, `IMapReader` para os storage modes do profile;
- implementação de `IGameActions` respeitando ABI e game-thread dispatcher descobertos pelo BuildMigrator.

Nenhum módulo de Healing/Targeting/Cavebot/Looter precisa mudar quando isso for ligado.
