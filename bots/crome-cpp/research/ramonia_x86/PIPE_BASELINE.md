# Padrão de busca x86 com Ramonia como referência

O catálogo em `src/RamoniaPipeBaseline.hpp` é usado pela busca de semantic binds, pelos diagnósticos estáticos de ABI e pela persistência de ações do probe x86. Ele mantém o trabalho atual como padrão, sem importar RVAs de outra build.

`RamoniaResearchProbe_V40.dll` aceita executáveis PE32/i386 independentemente do nome do cliente. Ramonia é a referência semântica, não uma lista de clientes permitidos. Em outra build, a pesquisa usa a fingerprint, RTTI, binds, layouts e capturas próprios; ausência ou ambiguidade mantém a evidência pendente. O profile confirmado do Ramonia só pode ser reutilizado com fingerprint exata e endereço redescoberto compatível.

A DLL é compilada para x86 e não pode ser injetada em um processo x64. Clientes x64 seguem o mesmo princípio usando o BuildExplorer e ABIBuilder de `reference/x64`. Os nomes históricos da DLL, pasta e schema são preservados para compatibilidade, sem restringir a descoberta pelo nome do executável. Remover a restrição de nome não torna a bridge específica do Ramonia compatível com uma build ainda não validada.

## Cobertura

- Game: autoWalk, attack, follow, look, move, use, useWith e talkChannel (talk preservado como alias histórico).
- Map/Tile: getTile, getCreatureById, getPosition, getThings, getThingCount, getTopMoveThing, isWalkable e isPathable.
- Item/inventário/containers: getInventoryItem, getId, getCount, getContainers, getItems, getContainer, getCapacity e getName.
- Layouts continuam sendo descobertos pelos passes existentes de Player, Creature, Inventory, Tile e Container.

`pipeBaseline` no JSON registra o catálogo e as políticas atuais. Ele não é uma autorização para executar comandos: os guards da bridge continuam responsáveis por isso. As funções adicionais da busca existente foram preservadas.

## Tile::getTopMoveThing

A função já fazia parte da busca. Agora também entra no diagnóstico de ABI, na lista de contratos persistentes e no resume/save do profile, incluindo retorno e refcount.

O profile canônico desta fingerprint registra a evidência já obtida no cliente:
ECX = Tile*, hidden return storage em [ESP+4] na entrada / [EBP+8] após o prólogo, RET 4, retorno shared_object_ptr<Thing>, incremento e liberação da referência em +4 e confirmação de pertencimento ao Tile::things. Fonte: diagnóstico topmoveat e sequência real 2384 -> 20159 -> 2389 no tile 1220,1280,7.

O probe preserva essa confirmação apenas se o profile tiver a mesma fingerprint e o bind redescoberto tiver o mesmo RVA e contrato completo. Se o bind mudar ou faltar, invalida o contrato antes de salvar checkpoints.

Em uma build nova, a busca registra discovery_required ou abi_validation_required. A captura opcional usa o breakpoint one-shot existente, observa uma chamada natural e verifica Tile RTTI em ECX e storage na pilha do caller. Entrada e análise estática sozinhas não confirmam retorno, cleanup ou ownership. Esses dados continuam pendentes até evidência de retorno/refcount e membership, como o diagnóstico topmoveat numa bridge compatível com a build. O probe não chama funções internas nem passa a mover itens.

O padrão do fluxo ativo é getTopMoveThing + validação do mesmo tile, um bloqueador por ação, releitura e alvo exposto antes de useWith. Não há seleção por stackIndex.

## Validação local

Compile a DLL com o script existente build_x86.bat, ou diretamente com MinGW i686. O número/schema V40 foi preservado; esta alteração adiciona um contrato ao catálogo, sem migrar o schema.

Teste de regressão:
```powershell
& C:/msys64/mingw32/bin/g++.exe -std=c++20 -O0 tests/PipeBaselineTests.cpp -o PipeBaselineTests_x86.exe
./PipeBaselineTests_x86.exe profiles/ramonia_x86/6AB03F94_E281E9CA5BA0AF2.json
```

O teste verifica resume por fingerprint, roundtrip do retorno/refcount e contratos move/useWith, invalidação de bind e busca ausente. A captura natural em uma nova build depende de teste real.

Crome.exe, bridge, comandos do pipe e Cavebot não foram alterados nesta etapa. Os arquivos x64 em reference permanecem referências com ABI própria; o contrato x86 não é aplicado a eles.
