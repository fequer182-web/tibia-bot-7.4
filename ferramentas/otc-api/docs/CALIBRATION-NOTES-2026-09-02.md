# Calibração e portabilidade — 2026-09-02

Registro das alterações, evidências e limites da sessão. O contrato continua em
[API.md](API.md); os comandos operacionais ficam no
[runbook](../configs/calibrations/README.md). Não interpretar uma calibração local
bem-sucedida como suporte completo a todas as ações ou prova em outra máquina.

## Resumo das alterações

| Área | Alteração | Verificação / limite |
|------|-----------|---------------------|
| Perfis | Leitura e gravação somente em `<dll>/calibrations/{buildId}.json`; sem override do jogo ou fallback para CWD | Save/load, pasta relocada, falhas e compatibilidade cobertos por teste; caminho confirmado no log Valantis |
| Build | Copia do catálogo fonte apenas perfis ausentes na Release | Teste confirma que rebuild não substitui calibração existente |
| P1 | Recuperação por referência Game quando a cadeia salva falha | Recuperação e reinício seguinte observados no Valantis, sem hints |
| P2 | Último passo do fallback via heap passa a dereferenciar; destino precisa ser igual à coleção descoberta | Código compilado; P2 voltou a funcionar no Valantis pela coleção direta no módulo, não prova isolada do fallback heap |
| Posição | Removidos cortes regionais arbitrários de X/Y; hints exatos, região opcional e validação por movimento | Faixa estrutural `0..65535`; Z `0..15`; não confundir posição plausível com posição que acompanha o player |
| P3 | `--z` filtra o andar sem usar `(0,0)` como centro de busca | X/Y desconhecidos usam sentinela distinta de zero válido; asserts de compilação cobrem os filtros |
| Compatibilidade anterior | Scans limitados, guard pages, layouts mistos e raw/shared por perfil; ações raw perigosas falham fechadas | Histórico DeusOT na skill; regressão Karma live continua pendente |
| Injector | Opção `--launch` cria processo suspenso, injeta e retoma; encerra o processo recém-criado se falhar | Código anterior incluído no conjunto; não foi validado ao vivo nesta sessão |

## Perfis: ciclo de vida, reset e distribuição

- P1 bem-sucedido seguido de `save` cria o JSON; `profile saved` confirma a gravação.
  Refine/P2/P3/P4/P5 atualizam o mesmo perfil. Inject e leitura da pipe não criam uma
  calibração inicial; a recuperação automática de uma cadeia existente pode salvá-la.
- Runtime de ambas as arquiteturas: `dist/calibrations/`. Catálogo aprovado para empacotar:
  `configs/calibrations/`. Promover somente após os testes da superfície pretendida.
- A DLL usa seu próprio diretório absoluto, não a pasta do jogo nem o diretório
  de onde o Node foi executado. O log `otc-api.log` continua na pasta do jogo.
- O build não sobrescreve perfis existentes. Uma atualização intencional exige
  backup e substituição explícita. Novos perfis fonte são copiados no pós-build da DLL.
- Em 02/09, por solicitação do usuário, oito perfis/cópias locais de Ramonia,
  Karma e DeusOT foram retirados das pastas de carga após backup com hash verificado.
  Valantis já estava sem JSON. Isso foi um reset local, não exclusão de fonte C++.
  Backups estão fora do Git, em `backups/`; não restaurá-los automaticamente.
- O catálogo fonte estava sem JSON nesta sessão. As matrizes históricas de Karma
  e DeusOT documentam evidência anterior, não garantem perfis incluídos no checkout.
- Distribuir app + DLL + injector + `calibrations/*.json`. Um único `.exe` exige
  empacotador/instalador no repo do app; essa funcionalidade não foi implementada aqui.
  O destino precisa usar o mesmo PE/buildId, arquitetura compatível e privilégios
  compatíveis entre jogo/injector/consumidor. Ainda falta validação em outra máquina.

## Valantis — falha P1 e recuperação comprovada

Build: `pe-61beeb3a-6a9365ea-00b47000-006cdc00-014c` (Win32).

O JSON era encontrado, mas `hasChain:true` coexistia com `statsBase:false` e
`needCalibration`. Cadeias de quatro e depois cinco passos funcionavam na sessão
da calibração e falhavam após reiniciar. Cinco passos não eram prova de estabilidade.
Uma leitura diagnóstica da cadeia no PID 25912 encontrou `0x5F53455A` onde deveria
haver um ponteiro; o passo seguinte era ilegível. Não era falha de localização do JSON.

A referência Game foi encontrada pelos metadados Lua de `talkChannel`. A primeira
versão da recuperação testava apenas o membro zero e não encontrou o player.
A leitura ao vivo no PID 1904 mostrou o LocalPlayer em `Game+4`, com HP 155/155,
mana 5/5, experiência 105, capacidade 271 e os equipamentos esperados.

A recuperação atual mantém a cadeia salva como primeira tentativa. Só após falha:

1. Usa candidatos Game descobertos no cliente, sem RVA ou nome de servidor no C++.
2. Examina membros de `0..64` bytes, alinhados à largura nativa do ponteiro.
3. Reutiliza o offset final de stats do perfil (positivo, até 8192, sem deref).
   Exige posição calibrada e inventário de ponteiros; não adivinha campos ausentes.
4. Confere vtable do player e dos itens não nulos, todos os slots legíveis, posição
   lida pelo layout, stats plausíveis e inventário não vazio. Objetos distintos
   válidos tornam o resultado ambíguo; aliases do mesmo objeto não.
5. Exige duas observações consecutivas da mesma referência, separadas por pelo
   menos um segundo. Não faz heap scan nem envia ações para o jogo.
6. Troca apenas `chain`, preserva os demais blocos e salva junto da DLL. Se a
   gravação falhar, informa no status; a recuperação em memória não prova persistência.

Evidência observada em 02/09 (horários locais do log):

| Processo | Resultado |
|----------|-----------|
| PID 4472, 16:05:32 | `P1 recovery via Game rva=0xA46EF0 member=4 statsBase=20C22D08 steps=3 saved=1`; pipe `ready` |
| PID 1216, 16:08:30, após fechar/reabrir | `pointer-chain resolved at 0x1DCB3520 (3 steps)` imediatamente; sem hints ou recuperação |

O segundo processo leu HP 155/155, mana 5/5, level 2, experiência 105, capacidade
271, posição `32097,32217,7` e inventário. O endereço do heap mudou. Uma comparação
do backup com o JSON recuperado confirmou que somente `chain` mudou.

Layout observado (evidência desse PE, nunca defaults globais): Game RVA `10776304`,
membro LocalPlayer `+4`, stats `+1200`; posição int32 em stats `-1188`; inventário
em stats `-144`, slots de 4 bytes, Item id/count `44/48`, `capacityScale=100`.
`soul=7` apareceu na pipe, mas não teve comprovação independente na UI; não tratá-lo
como campo validado só por ser plausível.

## Valantis — P2, P3, P4 e P5

- P2 falhava com `container-chain landed but read failed`. O fallback encontrava
  um campo contendo o ponteiro da coleção, mas devolvia `{off,false}`. Agora usa
  `{off,true}`, verifica a resolução e rejeita destino diferente do descoberto.
- Após a recuperação P1, o log do PID 1216 registrou em 16:10:03
  `container-chain resolved ... collection=map open=1 steps=1`, com save. O usuário
  confirmou que funcionou. Essa execução usou a coleção embutida no módulo.
  Ainda falta registrar o ciclo completo open/update/close e restart do P2.
- P3 no mesmo processo registrou `creature-chain resolved ... collection=map n=3
  steps=1` às 16:13:45 e novas resoluções posteriores. O perfil usa CreaturePtr 4,
  posição int32 em +12, id +84, nome +88 e hp% +112. Não confundir scan concluído
  com toda a sequência appear/update/disappear comprovada.
- P5 registrou `map-chain resolved mapRva=10776640 collection=map-blocks ...
  bs=32 thingsOff=36` às 16:25:22, com save. Os blocos P2/P3/actions/map existem no
  JSON local. Falta restart final de todas essas superfícies e teste de movimento
  do mapa depois dessa recalibração, antes de promover o perfil completo.
- Em teste anterior, `look 32095 32211 7` mostrou a mensagem correta no jogo e
  id 4535, mas a resposta trouxe `32211,7,0`. A leitura de posição do Thing em P4
  ainda tem offsets fixos; esse erro de coordenadas não foi corrigido nesta sessão.
- P5 anterior falhou com `Tile found but item id was not on that tile`; `--id 4535`
  funcionou quando esse item estava no tile. Hints de item podem persistir entre
  etapas na mesma DLL; usar um id conhecido do tile evita a ambiguidade. Não usar
  um id da bag como se fosse id do chão. Não foi implementada limpeza geral desses hints.
- `attackRva`, `followRva`, `chaseModeRva` continuam zero; não preencher manualmente
  nem considerar ataque/follow raw não nulos validados.

## Posição e hints: lições operacionais

Ramonia tinha posição estática aparentemente plausível (`5895,12632,7`), enquanto
a região real era próxima de `826,1869,7`. Cortes X/Y mínimos de 1000 impediam
calibração/ações nessa região; foram removidos, mantendo a faixa estrutural uint16.
No Valantis, reutilizar `--minX 100 --maxX 3000 --minY 100 --maxY 3000` excluía a
região real perto de 32 mil. Limites explícitos continuam válidos e devem ser atuais.

Use coordenadas exatas quando conhecidas. Caso contrário, `--refine --z 7 --moving`
exige caminhar durante a coleta no mesmo andar. `--moved` exclui o offset anterior
após um movimento. `hasPosition:true` sozinho não comprova que a posição acompanha o player.

No P3, liberar zero como coordenada válida expôs a colisão com a antiga sentinela
`(0,0,z)`. X/Y desconhecidos agora são `(-1,-1)`. `--z` filtra só andar; não exige
estar perto da origem. O uso normal com posição conhecida mantém o limite de distância.

## Verificações e pendências

- Build Win32: DLL e injector publicados em `dist/`; `profile_store_test` permanece no build interno `build/Release/`.
- CTest: `profile_store` e `seed_calibrations` passaram. Incluem cadeia Game com
  membro adjacente, limites/alinhamento, confirmação em dois polls, save/load,
  relocação, rejeição de CWD/traversal/buildId incompatível e proteção de rebuild.
- Oito backups históricos passaram por leitura, gravação e comparação sem mudança
  de layout. Isso é compatibilidade de serialização, não regressão live.
- P1 Valantis: recuperação e restart direto comprovados. P2/P3/P5: evidências de
  calibração acima; não declarar suporte completo apenas por existirem blocos no JSON.
- Karma live e outra máquina: pendentes. `--launch` e ações raw não nulas não ganharam
  selo de validação. Não fechar essas lacunas com build verde ou testes de arquivos.

```powershell
cmake --build build --config Release --target OTCApi OTCInjector profile_store_test
ctest --test-dir build -C Release --output-on-failure
```

Para testar backups locais sem restaurá-los ao runtime, opcionalmente execute
`build/Release/profile_store_test.exe <diretorio-de-backups>` (arquivos `.bak`).
Backups e binários não fazem parte do commit.

