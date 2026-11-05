# Recuperação de leitura do mapa — versão 5

Corrige o caso mostrado no painel: Mapa de criaturas ciclico/alterado.

Uma árvore lida por fora pode ficar inconsistente durante a leitura. O leitor continua rejeitando ciclos, nós inesperados, sentinela/contagem inválida, pares contraditórios e alterações do cabeçalho. A diferença é que esses casos descartam a leitura atual com WorldSnapshotUnavailable, em vez de registrar uma falha fatal do controlador.

O painel mantém os módulos ligados, preserva a luz e a rota já solicitada e tenta novamente com aproximadamente um segundo de intervalo. Nenhum candidato parcial da varredura é usado. Se o mapa continuar inválido, continua mostrando o motivo e tentando ler, sem enviar ações baseadas nessa leitura.

O evento fica no JSONL como map_scan_skipped, com reason e partial_candidates_used=false. A fase muda para Mapa indisponivel nesta leitura. Tentando novamente.

Isso não ignora falhas de chamada/restauração, build divergente ou dados críticos do jogador. Essas verificações continuam ativas. Não houve mudança nos cinco executáveis nativos nesta correção.

Foi testada uma árvore com um Rat válido seguido de um ciclo: a lista parcial foi descartada. Após reparar a árvore simulada, a leitura retornou o Rat. O controlador suportou duas leituras inválidas e a recuperação na terceira tentativa, mantendo flags, luz e o waypoint em andamento, sem chamar helpers. O teste anterior de falha nativa continua interrompendo as ações.

Para instalar: feche o painel anterior, extraia o pacote inteiro em outra pasta e execute iniciar.cmd. A sessão que já ficou com falha na versão anterior não é alterada automaticamente por esta entrega.

## Complemento V7

A verificação de Monster estabelece vtable, ID e estabilidade do par objeto/controle antes de classificar coordenadas fora do mapa. O nome só é lido para uma posição válida: ao sair do mapa, esse nome já pode ter sido apagado. Isso corrige o caso da imagem com name_length=0 e posição 65535,65535,255. O acompanhamento existente trata esse estado como perda de alvo vivo ou procura evidência adicional de corpo quando HP=0; não assume morte só pelo desaparecimento. Identidade realmente divergente continua rejeitada.
