# Aprendizado visual 7.4

Tools > Visual 7.4 observa o mapa por captura normal da tela. Esta etapa não
envia movimento e não habilita Auto Explore. As sugestões e evidências ficam
disponíveis para a próxima integração com o planejador de caminhos.

## Uso

1. Abra o cliente e o Crome e confira que o profile/posição estão disponíveis.
2. Mova o Crome para fora da área do mapa. Clique em Calibrar mapa.
3. No jogo, deixe o cursor no canto superior esquerdo do mapa e aperte Insert.
   Solte Insert, posicione no canto inferior direito e aperte novamente.
4. Confira as linhas da grade: padrão de 15 colunas por 11 linhas. Player C=7
   e L=5 são índices a partir de zero; altere se o personagem não estiver ali.
   Inclua só o mapa: não inclua Battle, bags, barras ou chat na área marcada.
5. Clique em Observar e volte ao jogo. As capturas são limitadas a uma por
   segundo; a comparação roda em segundo plano. Aguarda a posição ficar
   estável para alinhar o mapa. É possível andar normalmente durante a coleta.
6. Volte ao Crome para ver a captura anterior e clique no SQM desejado.
   Digite um nome e uma categoria e clique em Aprender SQM selecionado.
   Preferir um SQM sem criatura, texto ou efeito por cima. O ID cliente é
   opcional: ID do Look copia o último ID, sem afirmar que seja desse SQM.
7. Ler IDs do Tile usa a inspeção existente para a coordenada selecionada.
   Os IDs só aparecem quando a resposta corresponde àquela coordenada.
   Clique no ID para preenchê-lo. Uma pilha pode ter ground e outros itens;
   escolher o ID correto e a categoria continua sendo anotação do usuário.
8. Buscar imagem consulta o TibiaData pelo nome e baixa a imagem primária.
   Recarregar banco importa novamente as referências locais e do catálogo.
9. Parar salva o aprendizado. Também há gravação periódica e ao fechar.

Se redimensionar a janela do cliente, calibre novamente. Se perder o foco,
mostra a captura anterior para edição e suspende a coleta de evidências.
Não tenta contornar proteção de captura. Imagens vazias/uniformes suspendem
o reconhecimento; não confirmam piso transitável.

## Evidências e limites

- A chave local inclui timestamp, tamanho e hash do texto do profile, com
  prefixo 74. As amostras manuais ficam isoladas por build.
- A grade usa a posição validada do player e o centro configurado; uma
  calibração incorreta produz coordenadas incorretas. Alinhamento por SQM
  durante a interpolação do movimento não foi confirmado no cliente real.
- São registradas passagens, mudanças de andar, queda de HP >=20 e novos bits
  de estado. Combate observado exclui a atribuição automática de dano ao
  Tile. Essas evidências são candidatas: HP/estado não distinguem fogo,
  energy, poison, ataque não selecionado, teleport, escada ou buraco.
- Imagens são comparadas em RGB, normalizadas para 32x32. Pixels
  transparentes de uma referência são ignorados. Exige erro <=0,10 e margem
  >=0,025 para uma categoria diferente. Score não é probabilidade calibrada.
  Iluminação, animações, sobreposição e sprites diferentes podem dar
  desconhecido ou sugestão incorreta. Não é um modelo de IA treinado.
- A referência Wiki não é banco específico de sprites 7.4. O teste real
  Rope/asset 3439 funcionou; Fire Field retornou HTTP 404. A captura local
  permite completar os pisos da versão do OT sem depender desse catálogo.
- O ID de catálogo nunca é usado como ID cliente. Imagens consultadas
  mantêm clientId=0 e versão visual não confirmada. A amostra manual pode
  conter um ID informado pelo usuário, ainda sem validação automática.
- Limites: 512 referências, 50.000 coordenadas, imagem até 2048x2048,
  resposta HTTP até 8 MiB. Uma tarefa visual por vez. Resultados de uma
  captura/build/grade antiga são descartados após troca ou recalibração.
- As capturas ficam na memória, salvo o SQM que o usuário escolhe aprender.
  As imagens não são enviadas ao serviço; só o nome pesquisado.

Arquivos: visual74/<build>/learning.json e samples/*.bmp/*.json;
visual74/catalog/*.img/*.json. A pasta catalog é referência comum e não
promove categorias/IDs entre builds.

## Validação

Build Release x64 do EXE/DLL. Testes de grade não divisível, ambiguidade,
alpha, referências repetidas, imagens uniformes, coordenadas e evidência
conservadora de movimento. Testes Windows: captura sem cliente rejeitada,
escaping de nome e roundtrip BMP/WIC. Consulta HTTP Rope e asset real
decodificado no WIC. Previews ocultos de Visual, Tools, Compact e Targeting.
Captura/alinhamento no cliente, aprendizagem real e reconhecimento dos
sprites 7.4 continuam dependendo do teste do usuário.
