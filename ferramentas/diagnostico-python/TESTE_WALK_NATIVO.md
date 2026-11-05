# Identificacao da caminhada nativa — build atual

O executavel OpenGL do ZIP foi conferido pelo SHA-256 completo:

`fb6f5d30082258cf55969050e15ed0e500b1ddac7f47b1040eece4727cce7392`

## Evidencia encontrada no codigo

| Referencia | RVA |
|---|---|
| Candidato Game::walk | `0x140D60` |
| Candidato Game::autoWalk | `0x140700` |
| Instancia capturada pelo binding Lua de walk | `0xE52A40` |
| Construtor do callable usado no binding de walk | `0x1A9E10` |
| Adaptador que encaminha os argumentos | `0x1F6690` |

As funcoes foram delimitadas pelos registros de unwind `.pdata`. As referencias ao texto de debug `Game::walk` e `Game::autoWalk` caem dentro desses intervalos. O registro Lua associa o nome `walk` ao candidato `0x140D60`.

O construtor em `0x1A9E10` armazena o endereco da funcao em um campo e `base + 0xE52A40` em outro. O adaptador em `0x1F6690` carrega esses campos e encaminha um argumento de 32 bits por EDX e um byte por R8B, antes do despacho indireto para a funcao. Isso e evidencia estatica forte da identidade e da passagem de argumentos; o significado completo do terceiro argumento, o retorno e a thread correta continuam sem validacao em execucao.

## Por que nao usar diretamente o endereco anterior de Game

O Builder havia usado a regiao em `base + 0xE52A48` para ler os slots de jogador/alvos. Isso funcionou como referencia de leitura nesses testes, mas nao demonstra que esse endereco seja o `this` para uma chamada de metodo.

O callable nativo captura `base + 0xE52A40`. Nao se deve substituir um endereco pelo outro ao chamar uma funcao. O novo diagnostico mostra os primeiros ponteiros dessa instancia e os compara ao jogador resolvido pelo perfil, sem presumir qual estrutura interna eles representam.

## Testar agora no cliente

Extraia todos os arquivos juntos na pasta Cromin_py. Abra o jogo e entre no personagem, entao execute:

```powershell
python .\walk_native_probe.py --profile .\Antica_monitor.profile.json
```

Este teste e somente leitura. Ele confere a build, compara os bytes das funcoes e do binding com os do executavel analisado, le a instancia capturada e registra condicoes observadas no inicio de `Game::walk`. Salva `walk_native_probe_DATA_HORA.json` na pasta atual.

As condicoes incluem bytes em `this + 0x60` e `this + 0x62`, ponteiros em `this + 0` e `this + 0x30` e o byte `ponteiro0 + 0x69`. Sao apenas verificacoes vistas no prologo: existe tambem uma chamada interna de verificacao e outros caminhos nao avaliados pelo diagnostico. Mesmo `all_observed_entry_checks_pass: true` nao significa que uma chamada seja segura ou que o personagem tenha andado.

O resultado sempre informa `native_call_executed: false`. Ainda nao e um executor de caminhada. A tentativa local nao encontrou o Antica aberto, portanto esta etapa depende do resultado do teste na sua sessao.

Tres testes controlados passaram: leitura sem alteracao de memoria, rejeicao de bytes diferentes e registro de precondicao falsa. A analise estatica tambem encontrou referencias independentes pelo texto de debug, registro Lua e callable armazenado.

Envie o JSON para conferir os ponteiros antes de preparar o teste de execucao. Uma chamada nativa exigiria executar codigo no contexto do cliente e verificar o contexto/thread; ela nao pode ser realizada com `ReadProcessMemory` sozinho. Nesta etapa nao ha injecao, hooks, escrita no cliente nem envio de teclas.
