# Olders x64 Loot pelo Pipe

Esta implementacao e exclusiva da baseline historica cujo timestamp e 6AA9FA57,
SizeOfImage 17BF000 e hash de .text 89D3D35243890EA2. A verificacao existente
continua obrigatoria antes de instalar o dispatcher. Nao habilita outra build.

O Pipe SET_CONFIG salva config/loot.json; o worker recarrega em ate 750 ms.
Com Loot Items ligado, sem alvo de combate, follow manual ou interacao manual,
a cada 600 ms o dispatcher do game thread relê Tile::things e os containers.
Ele usa os Client IDs configurados, abre o corpse por Game::use, identifica
containers de corpse e move um item por Game::move para a bag aberta indicada.
Pausa o Walker durante a coleta e devolve o controle ao combate quando aparece
um alvo. Cada passo relê os ponteiros; nao conserva um ThingPtr entre chamadas.

Evidencias da baseline: profile ABIBuilder em reference/x64/ABIBuilder_V1_3/
profiles/OTCLab_BuildProfile_6AA9FA57_89D3D35243890EA2.json, layout Item/Container
confirmado e captura de Game::move (RVA 163E20), RCX=Game*, RDX=ThingHandle*,
R8=Position*, R9=count. A inspecao da funcao confirma que o argumento e emprestado:
o incremento/decremento interno balanceia sua copia, sem consumir o handle do
chamador. Game::use (164570) tambem usa o handle emprestado. Diferentemente do
attack, nao se adiciona uma referencia que essas funcoes nao vao consumir.

Validacoes: Item vtable 11DA0D8, container id 0..63, capacidade, deque completa,
origem codificada (65535, 0x40|containerId, slot), destino nao-corpse com vaga.
Containers paginados sao recusados. Bags internas so sao abertas para IDs da
lista, e sua origem precisa corresponder a um slot no container pai de corpse.
Nenhuma funcao close e inventada; nao altera move/open genericos nem outras ABIs.

Um pedido move nao equivale a confirmacao do servidor. Tres pedidos com o mesmo
item/count/slot/destino interrompem a coleta desse corpse por 30 s e registram
move_not_confirmed. Sem progresso de aproximacao, registra corpse_unreachable.
Falha de abertura tem timeout de 2,5 s, com intervalo de 30 s antes de nova tentativa.

## Teste real pendente

1. Feche o Crome antigo. Substitua Crome.exe e CromeRuntime.dll juntos, preservando
   config/ e profiles/. Reinicie o cliente para remover a DLL antiga da memoria.
2. Confira pelo Look o Client ID da moeda e do corpse. O ID 2148 salvo atualmente
   pode ser de outro esquema de IDs: nao e convertido automaticamente.
3. Abra primeiro a bag de destino, escolha seu Destination Bag ID no Loot
   (normalmente 0) e deixe vagas. O log informa IDs abertos se o destino faltar.
4. Configure Loot IDs e Corpse Client ID, ligue Loot Items e Only List. Comece
   parado ao lado de um corpse com uma moeda; confirme que so a moeda vai para
   a bag. Depois teste dois itens, bag cheia e a retomada do Cavebot.
5. Se falhar, leia as linhas [LOOT] no CromeRuntime.log. move_requested e apenas
   pedido; a prova final e a mudanca do item no cliente.

Limites atuais: nao fecha corpse/bag automaticamente (ABI close pendente), nao
junta pilhas numa bag cheia, nao usa Max Weight, Ground Drop ou Loot on Screen.
Feche corpses manualmente durante o teste; novas aberturas param ao atingir
16 containers. Bags internas exigem um vinculo verificavel ao corpse pai.
Build e testes locais nao substituem a validacao ativa no Olders.
