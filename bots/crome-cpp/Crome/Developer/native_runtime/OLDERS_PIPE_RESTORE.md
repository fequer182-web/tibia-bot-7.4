# Olders: restauracao do runtime historico pelo Pipe

O historico desta conversa registra que a versao de leitura (6689591) bloqueou
a injecao enquanto o catalogo novo estava pendente. O codigo antigo nao foi
removido: OldersInternalActions e olders_walker_test preservam o SDK v21.6.
O catalogo de descoberta x64 (ba9a015) ainda nao implementava o transporte Pipe.

Esta restauracao usa os wrappers e modulos existentes: caminhada/Cavebot,
targeting/ataque, healing, uso de itens, useWith no chao (rope/shovel/fishing),
fala/magias, food e as ferramentas ja executadas pelo worker. As funcoes stub,
anti-idle pendente, backend de move/equip e spear x64 nao ganham implementacao.
Nenhuma flag callable do ABIBuilder e promovida por esta restauracao.

A excecao historica e restrita a timestamp 6AA9FA57, SizeOfImage 17BF000 e hash
FNV1A64 do .text em disco 89D3D35243890EA2, em PE64/AMD64. UI verifica o profile,
personagem/vtable e executavel; DLL verifica o executavel novamente antes dos
wrappers e conserva validateRuntime. Outras builds ficam pendentes.

Pipe local: `Crome.Olders.<PID>`, protocolo 1. STATUS so sinaliza ready depois
do dispatcher instalado. UI nao trata DLL carregada como runtime conectado.
SET_CONFIG envia as configuracoes existentes e recebe confirmacao. A DLL grava
os JSONs atomicamente para os mesmos modulos existentes consumirem; esta versao
conserva o processamento legado por configuracao, nao cria comandos individuais
de acao no protocolo do Ramonia. Consultas/look results continuam nos JSONs.

Config names sao limitados; mensagens ate 256 KiB, clientes remotos rejeitados,
I/O com tempo limitado e cancelamento ao parar. Desconexao nao habilita modulos.
Nao altera configuracoes do usuario instalado durante o desenvolvimento.

Startup deixou de reabrir stdout/stderr do processo. Log e Pipe usam pasta nativa
Unicode; caminhos ANSI legados sao separados sem conversao filesystem incorreta.
Timeout de LoadLibrary nao libera memoria ainda em uso pela thread remota.
Essas correcoes nao confirmam a causa original do fechamento do Olders.

Validacao local: Crome.exe e CromeRuntime.dll compilados; olders_pipe_tests cobre
fingerprint, prontidao, entrega repetida por Pipe real, persistencia/ACK, caminho
com acento, rejeicao de traversal/config invalida e parada limitada. A inicializacao e as acoes reais dependem de teste
no Olders. Reinicie o cliente ao substituir a DLL e use a pasta nova do pacote.

## Correcao de ownership no ataque

O dump Olders_OpenGL.exe.17092.dmp registra heap corruption (0xC0000374) e
retorno CromeRuntime.dll+0x2FE89, imediatamente depois da chamada Game::attack
na DLL instalada. Na build exata, o caller natural incrementa Creature+8 para
o argumento em RVA 0x217CFE; Game::attack consome essa referencia no caminho
comum de retorno em RVA 0x16565F, inclusive nos retornos antecipados.

O wrapper passava um ponteiro emprestado sem adquirir essa referencia. Agora
adquire uma referencia atomicamente antes da chamada, rejeitando contador
invalido ou saturado. Nao libera novamente depois: o callee ja consumiu o
argumento. A chamada permanece no game thread e com as validacoes existentes.
Esta correcao e restrita ao ataque da baseline exata; nao promove outras ABIs.

olders_attack_ownership_tests simula o contrato consumido, retorno antecipado,
troca de alvo, 100 chamadas e contadores invalidos. Teste e DLL compilados com
sucesso; o ataque real corrigido ainda depende de confirmacao no cliente.

## Modos de posicionamento no combate

O menu Mode agora grava movement_mode no targeting.json e no Pipe. Indices:
0 follow (aproxima ate 1 SQM), 1 none (nao solicita movimento), 2 stand near
(mantem 1 SQM), 3/4/5 mantem 2/3/4 SQMs por distancia Chebyshev, 6 diagonal
(1 SQM nos dois eixos), 7 diagonal 3sqm (3 SQMs nos dois eixos).
Keep Follow foi substituido pelo proprio Mode, eliminando controles conflitantes.
O modo e restaurado ao abrir o Crome e ao carregar um profile; configs antigas
sem movement_mode mantem o comportamento legado no runtime.

Modos de distancia tambem recuam quando o monstro chega perto. O destino e o
ponto geometricamente mais proximo na borda escolhida; o caminho continua sendo
calculado/aceito pelo LocalPlayer::autoWalk existente, sem nova chamada nativa.
Nao verifica ataques do monstro ou garante protecao durante o percurso, nem
escolhe automaticamente outra diagonal se a escolhida estiver bloqueada.
O bloqueio de Follow existente, pausa do Cavebot, guard de interacao manual e
limites de frequencia continuam ativos. None nao cancela um caminho ja enviado.
Pathable IDs ainda nao altera o pathfinding do cliente nesta implementacao.

target_positioning_tests verifica todos os quadrantes da diagonal, anel de
distancia/afastamento, alvo em movimento, andar diferente, None e compatibilidade
de configuracao. Build/testes locais nao substituem a validacao no Olders.

## Change Gold e organizacao do Cavebot

Change Gold em Tools envia change_gold pelo tools.json/Pipe; os IDs fonte ficam
em Cavebot > IDs > Change Gold IDs (walker_ids.json, chave changeGold). Cadastrar
os Client IDs de gold e platinum, sem crystal. A lista inicia vazia e o modulo
desligado; o perfil/bundle preserva a lista e a opcao.

O runtime Olders procura uma pilha de exatamente 100 no inventario ou containers
abertos, ignorando pilhas menores mesmo quando aparecem antes da pilha cheia.
A busca e o Game::use existente executam no game thread; uma pilha por tentativa,
com nova leitura na proxima, intervalo de 1 segundo e dispatcher de itens comum.
Game::use da baseline empresta o handle (nao consome como Game::attack).
O servidor decide o resultado da conversao; retorno normal nao comprova sucesso.
Containers fechados nao sao abertos e moedas nao sao empilhadas automaticamente.
Esta implementacao e Olders x64; o checkbox fica desabilitado no Ramonia.

No Cavebot, IDs e salvar/carregar perfil ficam juntos no topo; Target, HP, Range
e Mode ficam alinhados em duas colunas. Should Attack Me indica a descoberta
pendente. change_gold_tests verifica configuracao, IDs invalidos/duplicados,
intervalo, pilhas cheias e compatibilidade dos outros usos de item.
Build local confirmado; disposicao visual e conversao real dependem do cliente.
