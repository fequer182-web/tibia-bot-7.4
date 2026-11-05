# Descoberta x64 como padrão para o pipe

Estas alterações ficam na pasta reference e usam o BuildExplorer x64 e o ABIBuilder V1.3 existentes. Não habilitam funções no Crome/Olders, não alteram o CromeRuntime e não instalam um servidor de pipe. Prepararam o contrato descoberto para a integração futura.

## Sequência

1. Execute o build.bat de BuildExplorer_V2_7_1_PlayerRebind e use sua DLL x64 no cliente.
2. Siga o fluxo de descoberta existente. O profile final inclui pipeDiscovery.
3. Termine/descarregue o Builder; use uma sessão limpa do mesmo cliente para a ABI, sem hooks antigos interferindo no hash.
4. Copie o JSON recém-gerado para a pasta profiles ao lado do ABIBuilder, conforme seu fluxo existente.
5. Execute o build.bat de ABIBuilder_V1_3 e use sua DLL x64.
6. Produza as ações naturais solicitadas. Nenhuma ação interna é chamada automaticamente.
7. Guarde o JSON atualizado: cada método tem discovery_required, abi_validation_required ou as evidências capturadas. callable continua false.

## Regras do padrão

Look e Talk usam famílias sem RVAs no catálogo compartilhado. O Builder exporta `pipeDiscovery.actionSelection`: Look normal/detalhado e Talk de canal/normal. A ABI escolhe somente entre candidatos únicos da fingerprint atual, observando duas chamadas naturais da mesma função. Se não houver hits, testa a alternativa separadamente; captura parcial não é misturada. A evidência salva mantém a identidade da função observada e nunca habilita automaticamente uma ação. Isso também vale em outras builds, sem transportar os endereços do Olders.

O catálogo compartilhado common/PipeDiscoveryCatalog.hpp lista 29 métodos de Game, Map, Tile, LocalPlayer, Item, Thing e Container. O Builder busca semantic binds na imagem carregada, usa owners redescobertos pelo resolver V4.9 e só registra um RVA selecionado se os candidatos convergirem em um endereço único. Os endereços legados do migratorState não são copiados para pipeDiscovery.

Funções ausentes, sem owner ou ambíguas permanecem pendentes. Uma busca por token é um candidato; ainda pode corresponder a um wrapper e exige validação de ABI.

O ABIBuilder exige timestamp, SizeOfImage e hash .text exatos, além do schema/arquitetura e da fingerprint do catálogo de descoberta. O hash usa a mesma extensão de .text do BuildExplorer (máximo de VirtualSize e SizeOfRawData). Não existe opção para ignorar um hash diferente.

Os RVAs usados nas capturas vêm exclusivamente de pipeDiscovery. Evidências anteriores só são preservadas para a mesma fingerprint e mesmo RVA recém-descoberto. Registros herdados são mantidos passivos: callable, contractComplete e activeCallValidated ficam false.

A captura adicional de getTopMoveThing/getTopUseThing é de entrada natural, com snapshots dos registradores e da pilha. Não confirma o retorno nem ownership. Esta etapa nunca copia ECX/RET4/sret do Ramonia x86 para x64. Retorno, lowering C++, referências e thread continuam exigindo evidência própria. Outros getters do catálogo ainda não têm classificadores completos de ABI: ficam pendentes, mesmo quando seus endereços foram encontrados.

As regras e capturas existentes continuam disponíveis. Profiles antigos precisam ser gerados novamente pelo Builder atualizado antes da ABI; não se deve adicionar endereços manualmente para passar os gates.

## Validação

Testes em ABIBuilder_V1_3/tests/PipeDiscoveryTests.cpp verificam unicidade, fingerprint exata, rejeição de profile legado sem nova busca, catálogo completo passivo e invalidação de prova quando o RVA muda. As duas DLLs x64 foram compiladas localmente. Busca e captura dentro do Olders dependem do teste real no cliente.

Próxima etapa após coletar evidências: completar os classificadores pendentes e implementar o consumidor/servidor do pipe x64 com gates por contrato validado. Este pacote não muda a integração nativa do Olders.
