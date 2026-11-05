# Look manual com ID ao lado do PID — versão 6

## Usar

1. Feche o painel antigo, extraia o pacote inteiro e abra iniciar.cmd. Há um novo executável OTC_Look_Watch.exe em native/bin; não copie somente o Python.
2. Clique no rodapé e conecte ao Antica. Para o primeiro teste, deixe as automações desligadas.
3. Dê o Look normal no jogo em um item ou corpo.
4. O rodapé mostra o último ID de item capturado, por exemplo: **Online PID: 29564 · ID: 3994**.
5. Para copiar o ID, clique com o botão direito no rodapé. O clique esquerdo continua abrindo Conexão.

Não existe aba nova de Look. A captura inicia automaticamente com a conexão e também funciona com as automações desligadas. Captura o objeto recebido pela chamada real de Look do cliente; não infere o ID por proximidade ou pelo último corpo morto.

Se o Look for numa criatura ou num tipo de objeto ainda não reconhecido, mostra Look: ? em vez de manter o ID anterior como se fosse o novo item. O ID de rede de um monstro não é exibido como ID de item. Reconectar cria um estado novo; o ID do último Look não é salvo nos perfis.

## Intervalos de captura

O observador usa depuração externa, sem DLL, sem patch de código e sem chamar Game::look automaticamente. Reserva breakpoint de hardware e restaura o registrador ao sair. Antes de executar uma ação nativa, o controlador cancela o observador e exige confirmação de restauração/detach. Retoma a observação no próximo pulse após a ação retornar.

Um Look ocorrido durante essa pausa pode não ser capturado; repita depois da ação. Se o observador estiver indisponível, a janela Conexão mostra o estado. Feche o painel antes de usar helpers manuais por fora dele, pois dois depuradores não podem anexar ao mesmo cliente ao mesmo tempo.

Fechar/desconectar para o observador. Se o processo do painel encerrar inesperadamente, o helper monitora o processo pai e tenta sair/restaurar. Não encerra o cliente ao sair. Nenhuma chamada nativa começa enquanto o observador não confirmar que liberou a depuração.

## Identificação e evidência

Na build conhecida, a ligação Lua de look aponta para Game::look em RVA 0x141C60, com instância Game em RVA 0xE52A40. O argumento RDX aponta para o par compartilhado do Thing. O leitor confere o par, a VTable de Item e lê o ID uint16 em +0x40. A build e assinatura de entrada são conferidas antes de anexar.

O evento manual_look é salvo no JSONL da sessão com tipo, ID, objeto, VTable, posição e thread. Capturar a chamada confirma o objeto pedido ao Look; não confirma que o servidor aceitou a solicitação.

O teste nativo próprio passou para ID 3994, execução normal do Look sem alterar argumentos, múltiplas capturas, cancelamento, restauração e detach. Testes Python passaram para rodapé, distinção entre monstro/item, pausa antes das ações, retomada e bloqueio em detach não confirmado. Nenhuma captura no Antica foi realizada por estes testes; falta validar com o seu Look real no jogo.

O Look não altera a lista de loot automaticamente. Os IDs coletados servem para configurarmos a identificação de outros corpos/itens na próxima etapa; o loot atual continua com as validações já existentes.
