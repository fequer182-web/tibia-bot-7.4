# OTCLab BuildExplorer V2.7 EXTERNAL PORT

Esta pasta agora inclui a descoberta guiada V2.8. O nome da pasta e do EXE
foram preservados para manter os caminhos existentes. Execute `build.bat`
para gerar `Build/OTCLab_BuildExplorer_V2_7_EXTERNAL_PORT.exe` atualizado.

Depois de selecionar o LocalPlayer, a nova etapa explica como mudar cada
atributo e compara os candidatos antes/depois. Confirma um campo por leitura
somente após duas mudanças reais e um único candidato. Testes impossíveis
agora podem ser pulados; suas pendências são guardadas em `guidedFields`.
Tipos double, float, uint32 e uint64 são considerados, sem escalas presumidas.
O formato antigo `localPlayer.fields` conserva somente candidatos double
únicos; consumidores antigos não passam automaticamente a ler outros tipos.

A entrada da descoberta deixou de exigir quatro atributos double simultâneos.
Vida máxima também serve de âncora estrutural se a vida mudar durante o scan.
Isso gera candidatos para investigação; não confirma o objeto nem uma ABI.
Escolha explícita continua necessária se houver vários LocalPlayers.

Teste local: compile `tests/guided_fields_tests.cpp` como C++17 x64 e execute.
O builder permanece externo e somente leitura. Não relogue durante a etapa
guiada; uma mudança detectada no caminho do personagem interrompe a leitura.

Port externo da linha V2.7 para clientes que nao aceitam a DLL/injecao.

## Objetivo

Manter o maximo possivel do fluxo de discovery usando somente um processo externo:

- fingerprint PE/.text;
- LocalPlayer por HP/Mana/XP/Level/Capacity;
- X/Y/Z e Direction por transicoes;
- pointer path direto e tentativa de pointer path indireto;
- Game owner quando o caminho do LocalPlayer permite;
- AttackTarget e FollowTarget por `0 -> Creature* -> 0` (opcional/guiado);
- Map Center por transicoes de coordenadas quando existir copia direta no modulo;
- scanner estatico das funcoes usando as assinaturas dos profiles de referencia V2.7;
- candidato estatico de `Game::look`/`Game::detailedLook`;
- Light por `BASE -> TOCHA -> BASE` e opcionalmente `BASE -> utevo lux -> BASE`;
- Runtime Watch externo;
- JSON final com tudo o que foi confirmado/candidato.

## O que NAO e identico a DLL V2.7

Sem rodar codigo dentro do cliente, o EXE nao observa `RCX/RDX/R8/R9` no instante do Look. Por isso o Look pode sair como `candidate_static_external`, mas o ABI runtime continua `pending_external_read_only`.

Tambem nao existe write/restore de Light nesta versao: o offset pode ser confirmado por transicao de leitura, mas `writeValidated` fica `false`.

Isso e intencional: este pacote nao tenta contornar bloqueio de injecao/protecao do cliente.

## Compilar

Execute `build.bat`.

Compilador esperado:

`C:\msys64\ucrt64\bin\g++.exe`

Saida:

`Build\OTCLab_BuildExplorer_V2_7_EXTERNAL_PORT.exe`

## Teste

1. Abra o cliente e entre no personagem.
2. Execute o EXE externo.
3. Informe o PID.
4. Informe os stats atuais.
5. Escolha o LocalPlayer que fizer sentido.
6. Siga X/Y/Z/Direction.
7. Deixe o programa tentar Direct/Indirect pointer path.
8. Se resolver Game owner, faca Attack e Follow guiados.
9. Opcionalmente teste Map Center.
10. O scanner le a `.text` remotamente e compara com `profiles\reference`.
11. Para Light use tocha e, se disponivel, `utevo lux`.
12. Envie o log e o JSON final.

## Look

Os IDs `3031` e `3552` continuam registrados no JSON como defaults de referencia para um teste futuro, mas nao sao tratados como universais.

O modo EXTERNAL nao instala breakpoint/hook. Se uma assinatura unica encontrar `Game::detailedLook` ou `Game::look`, o profile salva o RVA como candidato estatico e mantem o ABI pendente.

## Seguranca do teste

- `PROCESS_QUERY_INFORMATION | PROCESS_VM_READ` somente.
- `VirtualQueryEx` para enumerar regioes.
- nenhuma DLL;
- nenhum `WriteProcessMemory`;
- nenhum hook;
- nenhum breakpoint;
- nenhum bypass.
