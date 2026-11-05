# Crome Universal v7.8 - Look ID

- `Developer/`: código-fonte e build.
- `Crome/`: pasta final do usuário após executar `Developer\build.bat`.

Nesta fase o foco é capturar o Look real do cliente e mostrar `Look ID` no rodapé, com posição para itens no mapa.

v0.7.9 adiciona metadados profile-backed para Look ID e HackLight x64, com testes de calibracao. Documentacao: `Developer/docs/X64_LOOK_LIGHT_PROFILE.md`.

## v7.12 additions
See Developer/docs/changelog/CHANGELOG.md and Developer/docs/V7_12_TEST_GUIDE.md.

### v7.12 focus
- Profile Save/Load is the single persistence flow (includes Cavebot route and loot/fishing configs).
- Olders Look uses confirmed Game::detailedLook path.
- Fishing searches rod in equipment or open bags and randomizes water target.
