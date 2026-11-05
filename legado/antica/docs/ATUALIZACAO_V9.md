# V9 — Cavebot unico, bags 2853 e Auto Food

## Mudancas

- Cavebot tem somente Ativar cavebot. Enquanto ligado, volta ao primeiro waypoint ao terminar a rota. A caixa Repetir rota foi removida. Configuracoes antigas com loop=false tambem passam a repetir. Continua desligado ao abrir/conectar.
- Containers mudaram entre leituras deixa de interromper todas as automacoes. O leitor tenta ate tres pares de leituras; se nenhum par coincidir, ignora a leitura e mantem os modulos ligados. Nao usa o snapshot instavel.
- Loot abre automaticamente itens bag 2853 que encontrar dentro do corpo, em uma janela nova, e aplica as listas Recolher/Drop tambem ao conteudo dessas bags. Continua usando o numero da bag do jogador como destino; a bag do monstro e origem, nao destino. A bag 2853 nao precisa ser adicionada aos IDs dos corpos.
- Tools inclui Auto Food e o campo IDs de food, com 3577 (coxinha, conforme informado por voce) como padrao. A lista aceita IDs separados por virgula.

## Auto Food

Ative a caixa Auto Food em Tools. Ele procura os IDs configurados nos containers abertos, tanto do jogador quanto dos monstros. As bags de monstro abertas pelo Loot entram nessa leitura automaticamente. Nao le conteudo de uma bag que ainda esteja fechada e nao abre corpos sozinho quando o Loot estiver desligado.

Solicita um uso por vez, com pelo menos 15 segundos entre pedidos, e confere se o item desapareceu ou diminuiu. Se nao observar consumo, aguarda aproximadamente 60 segundos antes de novo pedido; nao presume que o personagem esta com fome nem que o servidor aceitou o uso. O campo de saciedade ainda nao foi identificado. Se o uso for recusado ou o item mudar antes da chamada, registra o motivo e aguarda.

Voce pode deixar food fora das listas de Recolher/Drop e ativar Auto Food para usa-la enquanto a bag do monstro estiver aberta. Se o mesmo ID estiver em uma regra de Loot/Drop, essa regra tambem pode remove-lo da origem; nao existe reserva de uma quantidade minima de comida nesta versao.

Auto Food comeca desligado, mesmo com configuracao salva. F8/Parar tambem desliga o modulo. Healing continua tendo prioridade e as chamadas sao serializadas, pausando o observador Look antes de cada helper.

## Bags dentro do corpo

Percorre bags 2853 ate quatro niveis e dezesseis aberturas por corpo, com limite total de 512 acoes por ciclo. Cada abertura exige o ID, objeto, slot, container e subtipo atuais e uma janela nova confirmada por leitura. Nao confunde uma janela da bag do jogador com a janela aberta para a bag do monstro.

Se a bag ja se alterou, a janela nao apareceu ou a abertura foi recusada, ignora o ciclo sem repetir uma chamada incerta. A versao nao fecha automaticamente as janelas abertas; verifique a quantidade de containers abertos em sessoes longas. As regras continuam podendo recolher ou descartar o proprio item 2853 se voce o listar explicitamente; primeiro tenta processar seu conteudo.

## Instalar e testar

1. Feche o painel anterior e aguarde os helpers terminarem.
2. Extraia o pacote completo em outra pasta. Abra Cromin_Projeto/iniciar.cmd. Existe um novo executavel OTC_Item_Use_Once.exe; nao copie apenas os arquivos Python.
3. Para manter sua rota e filtros, com os paineis fechados, copie a configuracao antiga para data/Cromin_Externo.settings.json.
4. Ligue o Cavebot e confirme que continua a rota depois do ultimo ponto, sem outra caixa de repeticao.
5. Teste Loot com um corpo contendo uma bag 2853 e confira a nova janela e a transferencia de seus itens.
6. Em Tools, confira 3577 e ative Auto Food. Teste primeiro com food em uma bag aberta do jogador e depois em uma bag aberta do monstro.

Para rodar os testes locais sem conectar ao Antica:

```powershell
python .\tools\testar.py
```

## Estrutura e validacao

- actions/loot_transfer.py: leituras estaveis com tentativas limitadas e erro recuperavel SkipCycle.
- actions/item_loot.py: abertura e percurso das bags 2853, limites e origem/destino conferidos.
- actions/contained_item.py: pedido unico de abrir bag interna ou usar food por identidade.
- actions/food.py: busca em containers abertos, intervalo, confirmacao por leitura e espera apos uso nao observado.
- core/state.py e core/controller.py: flag FOOD=64, scheduler serial, persistencia de food_ids e rota sempre circular.
- ui/panel.py: Cavebot com uma caixa e Tools com Auto Food/IDs.
- native/src/item_use_once.cpp: Game::open de item em container e Game::use na thread do jogo. Confere build, assinatura, thread, item e slot; restaura contexto/pilha e libera o depurador.

A ligacao use foi localizada no executavel de referencia em Game::use RVA 0x142260. A entrada usa Game* em RCX e uma referencia Item/Thing compartilhada em RDX. Game::open continua em 0x142EE0 com ContainerPtr vazio para pedir janela nova. Enderecos e argumentos foram analisados estaticamente nesta build; isso nao substitui o teste ao vivo.

Testes Python cobrem recuperacao do erro da imagem, modulos preservados, abertura da bag interna e transferencia de sua moeda, consumo observado de 3577 em bag do jogador/monstro, intervalo, espera sem consumo, perfil e interface. As fixtures nativas usam um processo proprio para conferir os argumentos, item, bag, contexto/pilha, detach e timeout sem chamada. Os seis executaveis anteriores permanecem iguais a V8. As novas acoes ainda precisam de validacao no Antica.

Os eventos auto_food e food_scan_skipped ficam no JSONL de sessao. Os relatorios de loot incluem opened_bags; o novo helper salva item_action_*.json e informa action=open_bag/use_food.
