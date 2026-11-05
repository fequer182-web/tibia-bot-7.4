# otc-api

API para OTClient no modelo Rifbot: **DLL injetada** lê a memória, expõe o mundo pela pipe `\\.\pipe\OTCApi`, pede ao **cliente** para agir.

Este repositório é **só a API** (C++ + injector + contrato JSON + perfis de calibração). O app (dashboard, scripts do jogador) é **outro repo**, consumidor desta pipe.

```
OTClient (RAM)  →  OTCApi.dll  →  pipe JSON  →  app (repo separado)
app             →  pipe JSON   →  DLL pede ao cliente agir
```

- **Ler:** objetos no cliente (`LocalPlayer`, containers, battle, mapa), layout no JSON do `buildId`.
- **Agir:** tecla/mouse ou função do OTClient. Sem montar pacote, sem XTEA.

Etapa 1 (P0–P5) **fechada** — baseline verde no KarmaOT_GL. O DeusOT atual está calibrado para toda a superfície de **leitura**; ações de criatura com ponteiro intrusivo continuam limitadas conforme a matriz abaixo. Features novas: campo na pipe ou ação que o cliente obedece, contrato em [docs/API.md](docs/API.md).

## Docs

| Arquivo | Para quê |
|---------|----------|
| [PLAN.md](PLAN.md) | Baseline P0–P5 (fechada) e regras de feature |
| [docs/API.md](docs/API.md) | Contrato da pipe — a superfície pública |
| [docs/CALIBRATION-NOTES-2026-09-02.md](docs/CALIBRATION-NOTES-2026-09-02.md) | Alterações, reset dos perfis, recuperação Valantis e evidências/pendências |
| [configs/calibrations/README.md](configs/calibrations/README.md) | Runbook completo para exe novo, perfil `{buildId}.json`, layout e ABI |
| [`.cursor/skills/otc-api-strategy/`](.cursor/skills/otc-api-strategy/SKILL.md) | Skill do agente — Rifbot, bans, features depois de P5 |
| [`.agents/skills/otc-api-strategy/`](.agents/skills/otc-api-strategy/SKILL.md) | Entrada equivalente da skill para Codex |

O agente lê essa skill em **todo** chat deste repo (regra `alwaysApply`). Não precisa invocá-la na mão.

## O que fica aqui / o que não fica

| Aqui (API) | Outro repo (app) |
|------------|------------------|
| DLL, injector, pipe | UI, editor de script |
| `configs/calibrations/{buildId}.json` | healer / cavebot / atalhos |
| Wizard de calibração (`tools/`, gera o perfil) | qualquer coisa que o jogador “usa” |

Código **deste** repositório, escrito do zero. `C:\Projetos\new-poc` não é fonte.

## Tools

### Calibração rápida de um exe novo

Use o [runbook detalhado](configs/calibrations/README.md#fluxo-completo-para-um-exe-novo). Durante a calibração, perfis são salvos em `calibrations/` ao lado da DLL, nunca na pasta do jogo. Depois de promovidos e recompilados, ficam incorporados à DLL; um JSON externo válido ainda pode substituí-los. O pacote consumidor não precisa distribuir a pasta de perfis. Regras que não podem ser puladas:

Finalize a incorporação com `npm run calibration:embed:win32 -- --BuildId <id>`
ou `npm run calibration:embed:x64 -- --BuildId <id>` conforme a arquitetura.
Para descobrir `<id>`, injete a DLL, execute `node tools/pipe_test.js` e copie o
valor completo de `data.buildId` no primeiro evento `dllStatus`. Também é possível
listar perfis locais com `Get-ChildItem dist/calibrations/*.json`.

- personagem logado no mapa; jogo, injector, Node e Monitor no mesmo nível de privilégio;
- um consumidor da pipe por vez;
- perfil novo com o `buildId` emitido pela DLL, nunca cópia renomeada de outro exe;
- P1 com todos os valores atuais e `chainSteps>0`; depois refine, P2, P3, P4 seguro e P5;
- restart sem hints antes de promover o JSON para `configs/calibrations/`;
- depois de mudar scan/read/action C++, regressão ao vivo de um cliente já verde.

```powershell
dist/OTCInjector-Win32.exe --name <Cliente.exe>
node tools/p1-calibrate.js --health <atual> --maxHealth <max> --mana <atual> --maxMana <max> --level <level> --exp <exp> --cap <cap-livre>
node tools/p1-calibrate.js --refine --x <x> --y <y> --z <z> --inventory --id <client-id> --slot <1-10>
node tools/p2-calibrate.js --name <janela> --capacity <slots> [--id <client-id>]
node tools/p3-calibrate.js --name Rat [--hpperc <0-100>]
node tools/p4-test.js --look <x> <y> <z>
node tools/p5-calibrate.js
```

Calibradores precisam de acesso exclusivo à `\\.\pipe\OTCApi`; feche o Monitor durante esses comandos. Somente o perfil ao lado da DLL é usado. O log continua na pasta do jogo.

Se o player aparece antes de reiniciar, mas não depois, isso não prova erro do operador:
a cadeia salva pode ser temporária. A recuperação automática mantém o layout e pode
reancorar o P1 pela referência Game validada. Após `LocalPlayer recovered; profile chain repaired`,
reinicie novamente e exija `pointer-chain resolved` sem scan antes de seguir.

### Comandos por etapa

Calibração (char no mapa):

```
node tools/p1-calibrate.js --health … --level …
node tools/p1-calibrate.js --refine --z 7 --battle --inventory
node tools/p2-calibrate.js --name Backpack --capacity 20
node tools/p3-calibrate.js --name Rat
node tools/p5-calibrate.js
node tools/p5-calibrate.js --id 351
```

(`--name` ignora maiúsculas; `--capacity` = slots da janela, não a cap do char.)

### Estado do DeusOT atual

`pe-a6b7516c-6a8eddb3-01ba3000-0121a200-014c` teve stats/posição/states, inventário, containers, criaturas e mapa validados ao vivo. Essas coleções usam ponteiros crus de 4 bytes nessa build; o [runbook](configs/calibrations/README.md#deusot--build-atual-calibrado-para-leitura) registra o layout. Os perfis locais foram removidos com backup no reset solicitado em 02/09; o catálogo fonte está sem JSON. Evidência histórica não significa perfil incluído no pacote atual.

`attack`/`follow` com alvo não nulo ainda não estão verdes nesse ABI intrusivo: seus RVAs ficam `0` e a DLL falha fechada em vez de travar o cliente. O caminho `shared_ptr` já verde no Karma permanece separado e intacto.

Pipe e P4 (cliente injetado):

```
node tools/pipe_test.js
node tools/pipe_test.js --only containers
node tools/pipe_test.js --only creatures
node tools/pipe_test.js --only map
node tools/p4-test.js --say exura
node tools/p4-test.js --attack <id P3> --follow
```

## Build

Win32 (cliente 32-bit, ex. KarmaOT):

```bash
cmake -B build -A Win32
cmake --build build --config Release --target OTCApi OTCInjector
```

x64:

```bash
cmake -B build-x64 -A x64
cmake --build build-x64 --config Release --target OTCApi OTCInjector
```

Os artefatos finais ficam todos em `dist/`: `OTCApi-Win32.dll`, `OTCApi-x64.dll`,
`OTCInjector-Win32.exe` e `OTCInjector-x64.exe`. Use o par da mesma arquitetura
do cliente.
Em x64, o calibrador distingue ponteiro cru de 8 bytes de `shared_ptr` MSVC de 16
bytes e salva a largura observada no perfil específico do `buildId`.

O CMake usa `build/` e `build-x64/` somente como diretórios internos. A saída
executável padronizada é sempre `dist/`; não criar `ReleaseNew`, `ReleaseNew2`, etc.

Se o link falhar (`LNK1104` / DLL em uso), confirme que um processo do jogo está carregando a DLL desse destino. O usuário autorizou permanentemente em 07/09/2026 o encerramento desse processo sempre que bloquear o rebuild, sem pedir nova confirmação. Antes de cada encerramento, confira o PID atual e o caminho absoluto da DLL carregada; encerre somente esse PID, nunca pelo nome ou por associação com o projeto. Recompile no mesmo diretório da arquitetura e avise que o cliente precisará ser reaberto e reinjetado.

Exemplo Win32, depois de confirmar o caminho da DLL nos módulos do PID:

```powershell
Get-Process -Name <nome-sem-.exe> | Select-Object Id,ProcessName,Path
taskkill /F /PID <pid-confirmado>
cmake --build build --config Release --target OTCApi OTCInjector
```

Depois abrir o cliente, logar, e injetar:

```
dist/OTCInjector-Win32.exe --name KarmaOT_GL.exe
```

Injector: `--pid`, `--name`, `--path`, `--dll`, `--list`, `--launch <caminho-do-exe>`.
`--launch` cria o processo suspenso, injeta e retoma; em falha de injeção/retomada,
encerra apenas o processo recém-criado. Esse modo não foi validado ao vivo nesta sessão;
o fluxo de calibração usa o jogo aberto no mapa e `--name`/`--pid`.
Sem auto-detect por nome de servidor. Log da DLL: `otc-api.log` na pasta do `.exe` do jogo.

Para DeusOT, o processo continua sendo escolhido explicitamente pelo operador:

```powershell
dist/OTCInjector-Win32.exe --name DeusOT.exe
```

Isso não cria conhecimento de servidor na DLL; o runtime seleciona o layout pelo fingerprint PE (`buildId`).

