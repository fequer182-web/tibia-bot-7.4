# Estado do projeto

## Ramonia x86

Profile ativo:

`research/ramonia_x86/profiles/ramonia_x86/6AB03F94_E281E9CA5BA0AF2.json`

Fonte canônico atual:

`research/ramonia_x86/src/RamoniaResearchProbe.cpp`

Versão real do fonte: **V33.1**. O nome histórico `V4_9_DIRECT_BIND` foi removido da árvore ativa durante a consolidação, sem mudança funcional no código.

Fingerprint confirmado no profile:

- arquitetura: x86
- timestamp: `0x6AB03F94`
- SizeOfImage: `0xAA2000`
- EntryPoint RVA: `0x47F460`
- .text FNV1A64: `0xE281E9CA5BA0AF2`
- renderer: OpenGL

Owners atuais:

- Game: `0xA2D4D0`
- Map: `0xA2D600`

Ações semânticas registradas no profile:

- autoWalk: `0x76860`
- attack: `0x79190`
- follow: `0x79300`
- look: `0x77640`
- move: `0x77920`
- use: `0x780D0`
- useWith: `0x784F0`
- talk: `0x796F0`
- talkChannel: `0x79750`

Tile::things confirmado no profile:

- begin: `+0x24`
- end: `+0x28`
- capacity: `+0x2C`
- element stride: `4`
- Tile vtable RVA: `0x774208`

O serializer do profile ainda usa o identificador legado v27. Isso foi mantido nesta consolidação para não misturar mudança estrutural com mudança de schema.

Próximo marco: validar o runtime ABI capture V33.1 começando por `autoWalk`, mantendo chamadas internas desabilitadas.
