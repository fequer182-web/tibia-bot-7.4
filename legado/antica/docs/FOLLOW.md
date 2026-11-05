# Follow e Tools simplificado — versão 4

Atualização V7: o Follow agora também pode ser renovado durante o ataque já ativo, quando o jogador para longe do alvo. Veja [ATUALIZACAO_V7.md](ATUALIZACAO_V7.md). A descrição abaixo do setter no início do ataque permanece válida. Desmarcar Follow impede novas renovações, mas não cancela o que já foi solicitado.

## Usar

1. Extraia o pacote completo e abra iniciar.cmd. O helper de ataque foi atualizado junto com o Python.
2. Clique no rodapé para abrir Conexão e conectar ao Antica.
3. Em Targeting, configure os nomes ou Qualquer monstro. Follow (perseguir o alvo) começa marcado.
4. Clique Aplicar e ative Target. No primeiro teste, deixe os outros módulos desligados e observe se acompanha o alvo quando ele se afasta.
5. Em Tools existe somente Light Hack. O valor é fixo em 31, sem campo de edição.

Follow é aplicado no próximo ataque solicitado pelo painel. Alterar a caixa não troca nem cancela um ataque já em andamento. Desmarcado escolhe DontChase no próximo ataque; marcado escolhe ChaseOpponent. Parar impede novas solicitações, mas ataque/perseguição já enviados podem continuar no jogo. O modo de combate escolhido permanece no cliente; o encerramento do painel não o restaura automaticamente.

## Implementação

O Follow da interface usa o modo de perseguição durante combate, por Game::setChaseMode, sem enviar Game::follow separadamente. Os valores 0 (DontChase) e 1 (ChaseOpponent) são definidos no [código do OTCv8](https://github.com/OTCv8/otcv8-dev/blob/master/src/client/const.h). Isso é uma referência de semântica; o endereço e campo desta build foram conferidos no executável Antica de referência.

Evidências estáticas da build conhecida:

- A ligação Lua de setChaseMode aponta para RVA 0x145600, com instância Game em RVA 0xE52A40.
- getChaseMode em RVA 0x167480 lê o DWORD em Game + 0xB0.
- A função setChaseMode recebe o modo em EDX, compara/atualiza Game + 0xB0 e chama o envio dos modos de combate.
- O helper confere a build e assinaturas de entrada do setter/getter antes de usar esses endereços.

Sequência: conferir jogador/alvo → aplicar modo se diferente → confirmar campo por leitura → obter referência própria atual do monstro → conferir identidade/modo novamente → atacar → restaurar contexto/pilha e liberar depuração. Se o modo já corresponde ao solicitado, não chama o setter novamente. Se a função não aplicar o modo, restaura e não envia ataque; não tenta contornar a recusa.

O relatório nativo inclui chase_mode_requested, chase_mode_before, chase_changed e chase_verified. chase_verified confirma o campo local, não confirma movimento nem aceitação pelo servidor. Ainda é necessário conferir a perseguição ao vivo.

## Luz

A UI não permite editar intensidade. Estado, aplicação e carregamento de perfis usam 31. A classe interna de escrita continua validando a faixa e restauração; a opção Tools apenas ativa/desativa. O efeito visual do byte candidato de luz continua sem validação nesta conversa. Diagnósticos ficam na janela Conexão e nos relatórios.

## Testes

Passaram fixtures próprias de chase ligado, desligado, já aplicado (sem chamada redundante) e rejeitado (sem lookup/ataque, com restauração). Também passaram lookup/ataque com referência própria, liberação sem ataque e timeout sem chamada. O teste Python confirma repasse do Follow para Rat e outros Monster, exige readback nativo, não repete em falha, migra luz antiga para 31 e preserva estado inicial desligado. O Targeting anterior foi confirmado por você; este Follow ainda precisa de teste ao vivo no Antica.
