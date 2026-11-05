# OTCLab BuildProfile v2 - contrato do consumidor

Obrigatório: `schema=otclab-build-profile-v2`, `schemaVersion=2`, `build`, `values` e `migratorState`.

O consumidor usa:
- `build.fingerprint` para seleção automática;
- `values.localPlayer.fields` e `values.creature.fields` para layouts tipados;
- `migratorState.map*`, `tile*`, `inventory*`, `container*` para estruturas do mundo;
- `migratorState.fn*` para RVAs confirmados;
- `migratorState.sig*` como fallback futuro de resolução;
- `status/confidence/method` para não tratar candidato/pending como confirmado.

Novos campos podem ser adicionados sem quebrar a v6 porque `BuildProfile::raw` preserva todo o JSON original.
