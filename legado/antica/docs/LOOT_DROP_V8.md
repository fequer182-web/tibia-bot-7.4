# Loot e Drop por IDs — V8

Atualizacao V9: o loot agora abre bags 2853 dentro do corpo e Tools inclui Auto Food. Leia ATUALIZACAO_V9.md; os limites de bags fechadas da V8 abaixo sao historicos.

## Configurar

Na secao Loot, preencha:

- **Bag de destino:** numero da janela do container aberto que recebera o loot (0 e a primeira janela; nao e o ID do item bag).
- **IDs dos corpos:** IDs de itens dos corpos que podem ser abertos. O valor antigo 3994 permanece como padrao.
- **IDs para recolher:** itens enviados para a bag escolhida. Padrao: 3031.
- **IDs para Drop:** itens tirados somente do corpo e jogados no SQM onde o personagem estiver no momento da chamada. Essa lista comeca vazia. A bag do personagem nao e varrida para descarte.

Separe varios IDs por virgula, espaco ou ponto e virgula. Use IDs numericos de 1 a 65535. O mesmo ID nao pode aparecer em Recolher e Drop: o painel pede para corrigir a lista antes de aplicar. Itens fora das duas listas ficam no corpo. Uma lista vazia nao executa aquela acao; com Recolher e Drop vazios, nenhum corpo e aberto pelo loot.

Use o Look normal no cliente para descobrir os IDs. Eles aparecem ao lado do PID; botao direito no rodape copia o ultimo ID. Clique Aplicar depois de editar e ative Loot. As novas listas valem para os proximos ciclos/corpos; um corpo ja processado nao e automaticamente reaberto para repetir transferencias.

O modo compacto tem rolagem. Clique **+** para ver os campos com mais espaco. As listas sao salvas nas configuracoes e nos perfis O/S. Configuracoes antigas recebem corpo 3994, loot 3031 e Drop vazio. Todas as automacoes continuam iniciando desligadas.

## Comportamento

O leitor busca corpos adjacentes com os IDs informados e passa o objeto exato ao helper de abertura. Quando integrado ao ataque, acompanha a identidade do monstro selecionado, inclusive outros nomes alem de Rat, e exige um corpo novo no ultimo tile valido. A busca independente de loot tambem pode processar corpos configurados proximos quando nao houver combate ativo.

Os itens de Drop sao processados primeiro. Para recolher, o destino pode ser qualquer container aberto valido escolhido pelo numero da janela; deixou de exigir o item bag 2853. Para Drop sem itens de Recolher, nao e necessario ter bag de destino aberta.

Cada transferencia confere corpo, janela, slot, item e identidade, chama Game::move uma vez e observa o resultado antes da proxima. No Drop, le o tile atual do jogador, reconfere a posicao na thread do jogo e confirma o item nesse tile junto com a alteracao no corpo. Se o jogador mudar de SQM antes da chamada, ignora a tentativa. Uma parede/recusa do servidor ou resultado nao confirmado nao gera repeticao cega da chamada.

Moedas 3031 continuam usando a quantidade de pilha ja validada. Outros IDs usam uma unidade por chamada: valor bruto de subtipo/cargas nao e tratado automaticamente como quantidade. Assim, uma pilha pode precisar de varias chamadas. Itens com cargas/subtipo sao confirmados pela remocao do objeto e pela aparicao de outro com o mesmo valor bruto no destino. Quando ha uma reducao observada da pilha, confere a unidade correspondente no destino. O limite e de 512 transferencias por corpo.

Para IDs sem empilhamento previamente validado, a bag precisa de um slot livre. Esta versao nao tenta encaixa-los numa pilha quando a bag ja estiver cheia. Os containers internos de uma bag nao sao abertos recursivamente. Use/rope/shovel e pesca continuam fora desta atualizacao.

## Instalar e testar

1. Feche o painel anterior e aguarde as chamadas em andamento terminarem.
2. Extraia o ZIP inteiro em outra pasta e abra Cromin_Projeto/iniciar.cmd. Python e os helpers de abrir/mover foram alterados juntos.
3. Para manter a rota e os filtros, copie a configuracao antiga para data/Cromin_Externo.settings.json, com os paineis fechados.
4. Teste inicialmente com um corpo cujo ID foi conferido pelo Look e um item que deseja recolher.
5. Depois acrescente um ID a Drop e confira que saiu do corpo e apareceu debaixo do personagem. Nao coloque o mesmo ID nas duas listas.

Para executar os testes locais sem conectar ao Antica:

```powershell
python .\tools\testar.py
```

## Estrutura e evidencias

- core/loot_policy.py: validacao das tres listas e migracao de configuracao.
- ui/panel.py, core/state.py, core/profiles.py e core/controller.py: campos, persistencia e encaminhamento das regras.
- readers/world.py: itens no tile e corpos filtrados por IDs.
- actions/item_loot.py: abertura configurada, transferencia, Drop e confirmacao por observacao.
- actions/combat_cycle.py: associacao do corpo configurado ao monstro acompanhado.
- native/src/corpse_open_once.cpp: abertura de um ID de corpo explicito.
- native/src/loot_once.cpp: ID de item explicito e destino container ou tile atual, com nova conferencia no checkpoint.
- reports/: JSONs das chamadas e evento attack_loot_cycle/standalone_loot no JSONL. Cada transferencia registra action=take/drop, item_id, count_requested e confirmed.

O comando legado tools/loot_once.py continua sendo o diagnostico de uma transferencia de moeda. O loot configuravel e acionado pela interface e pelo controlador.

Testes locais cobriram listas invalidas/conflitantes, migracao, persistencia, corpos e bags com outros IDs, itens nao permitidos, moeda, item unitario, pilha de outro ID, cargas/subtipo, Drop no tile do jogador, tile ausente e resultado nao confirmado sem repetir. As fixtures nativas conferem a abertura por outro ID, o destino no chao, identidade, mudanca de jogador/tile, contexto/pilha e detach. Os novos itens e o Drop ainda precisam de validacao ao vivo no Antica.
