# Atualização V7 — Follow automático e rota em repetição

## Mudanças

- Targeting com Follow marcado: se o jogador ficar no mesmo SQM por 2 segundos, longe mais de 1 SQM do alvo vivo no mesmo andar, tenta renovar a perseguição do mesmo ataque.
- A verificação continua depois de o jogador já ter andado e parado. Não depende apenas do início do ataque.
- Cada renovação usa ChaseMode 0 → 1. Não chama Game::attack de novo, não troca o alvo e não envia teclas.
- Há pelo menos 3 segundos entre tentativas, com até 3 tentativas enquanto continuar parado naquele ponto. Andar para outro SQM reinicia essa contagem. Encostar no alvo interrompe as tentativas.
- Confere novamente ID, objeto, nome, controle da referência, ataque ativo e andar no helper. Se o alvo mudou antes da chamada, ignora a tentativa.
- Ao ligar o Cavebot pela interface, Repetir rota é marcado automaticamente, mesmo com configuração antiga que tenha repetição desligada. Ao chegar ao último waypoint, retorna ao primeiro. Ainda pode desmarcar Repetir rota durante a execução para terminar a volta.
- Corrige o erro da imagem: criatura com posição 65535,65535,255 e nome apagado passa pela verificação de identidade do objeto e é classificada como fora do mapa. Um alvo vivo nesse estado é descartado sem abrir corpo nem desligar as automações. HP zero continua exigindo a confirmação anterior de corpo novo para permitir loot.

Todos os módulos continuam desligados ao abrir/conectar. A opção Follow precisa estar marcada para renovar a perseguição. A rota é de caminhada, no mesmo andar; ferramentas não foram adicionadas nesta atualização.

## Instalar e testar

1. Feche o painel anterior e aguarde os helpers terminarem.
2. Extraia o ZIP completo em uma pasta nova. Abra Cromin_Projeto/iniciar.cmd. O Python e o helper OTC_Rat_Attack_Once.exe foram alterados juntos.
3. Para manter sua rota, com os painéis fechados, copie apenas data/Cromin_Externo.settings.json da versão anterior para data/ desta versão.
4. Conecte, marque Follow em Targeting e ative Target. Observe um alvo que se afasta: ao parar longe dele, o painel deve tentar renovar a perseguição após 2 segundos.
5. Ligue o Cavebot com uma rota gravada. Confira Repetir rota marcado e o retorno ao primeiro waypoint depois do último.

O setter pode confirmar o modo local sem que o cliente consiga percorrer o caminho. Uma parede continua podendo impedir a caminhada. Esta versão não cria uma rota alternativa própria até o monstro.

## Arquivos e diagnóstico

- src/cromin/actions/follow_watch.py: posição observada, temporização, limite e pedido de renovação.
- src/cromin/core/controller.py: compartilha o observador entre os ciclos e o ataque já ativo. Continua pausando o observador Look antes de chamadas nativas.
- src/cromin/actions/combat_cycle.py e target_attack.py: entregam jogador/alvo atuais ao observador.
- native/src/rat_attack_once.cpp: modo --refresh-chase 1; valida o ataque atual e faz somente a sequência 0 → 1 na thread do jogo, restaurando contexto/pilha.
- src/cromin/readers/world.py: classifica coordenadas fora do mapa antes de tentar ler o nome já apagado, sem aceitar identidade divergente.
- src/cromin/ui/panel.py: liga a repetição junto com o Cavebot.
- reports/external_session_*.jsonl: eventos follow_recovery, incluindo chase_renewed_movement_not_confirmed, stationary_retry_limit e preflight_skipped. Os relatórios nativos registram follow_refresh_only, follow_refresh_reset_verified e follow_refresh_returned.

## Validação

Testes Python locais: parada após movimento, intervalo, limite, alvo trocado, outro follow ativo, proximidade, Follow desligado, restauração não confirmada, recusa sem nova tentativa, nome apagado fora do mapa e retorno ao início da rota. Regressões de Look, mapa, combate, loot, healing, luz e interface passaram.

O teste nativo em processo próprio confirma 0 → 1 na ordem correta, nenhum lookup/ataque adicional, referências preservadas, restauração e detach, além de recusa na primeira e na segunda chamada. As funções continuam conferindo a build conhecida. A movimentação desta atualização ainda precisa de teste ao vivo no Antica.
