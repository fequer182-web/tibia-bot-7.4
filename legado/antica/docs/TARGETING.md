# Targeting por nomes e qualquer monstro

## Como testar

1. Feche o painel antigo e extraia esta entrega em outra pasta.
2. Execute iniciar.cmd. Clique no rodapé do painel e conecte pela janela Conexão.
3. Abra Targeting. No primeiro teste deixe Loot, Cavebot e Healing desligados.
4. Para filtrar, escreva por exemplo `Rat, Bat, Snake` em Nomes e deixe Qualquer monstro desmarcado.
5. Clique Aplicar e depois marque Ativar Target.
6. Para atacar qualquer monstro válido do mesmo andar, marque Qualquer monstro e clique Aplicar.

A lista usa nomes completos e não diferencia maiúsculas: `bat` encontra `Bat`, mas `Dragon` não inclui `Dragon Lord`. Aceita até 32 nomes. Nomes repetidos são removidos. Qualquer monstro ignora a lista de nomes. O padrão continua sendo somente Rat; as automações continuam iniciando desligadas.

O alvo é o mais próximo entre os candidatos válidos do mesmo andar. Depois de solicitar um ataque, acompanha o mesmo ID/objeto. Não troca alvo enquanto o cliente mantiver ataque/follow ativo. Uma mudança de filtro vale para a próxima seleção; não cancela o ataque já enviado. Parar impede novas chamadas, mas o combate já solicitado pode continuar no cliente.

## O que significa qualquer monstro

Aceita objetos da classe Monster da build conhecida, com identidade, nome, vida e posição válidos. Não inclui jogadores ou NPCs. Objetos em cache com posição FFFF, mortos ou outro andar não entram na seleção. A leitura suporta o nome curto inline e nome longo por ponteiro.

O executável nativo recebe ID, objeto e nome observado codificado em hexadecimal UTF-8. Confere novamente esses dados antes do ataque e antes de consumir a referência obtida pelo mapa. O filtro não é apenas um campo da interface: a restrição fixa ao nome Rat foi generalizada no helper também.

## Loot nesta entrega

O ciclo completo conhecido continua para Rat/corpo 3994/moeda 3031/bag 2853. Para outro monstro, solicita o ataque e acompanha a saída do mapa; não tenta adivinhar o ID do corpo. `target_left_world` não confirma morte nem loot. A seleção por nomes não equivale a loot genérico por IDs.

## Relatórios e configuração

A configuração salva target_names (lista) e target_any (booleano). Arquivos antigos sem esses campos carregam Rat e false. O/S também mantém esses campos.

Os relatórios de sessão guardam o alvo selecionado e o resultado da chamada. Os nomes de arquivo OTC_Rat_Attack_Once.exe e os campos rat_id/rat_object foram mantidos por compatibilidade; no modo novo podem representar outro monstro validado. `confirmed` em target_attack confirma o ataque/restauração, não a morte. `death_confirmed` permanece false quando o alvo apenas saiu do mapa.

## Validação

Passaram testes locais de filtros, nomes longos, leitura de Monster, exclusões, persistência/migração, chamada única, identidade trocada e falha sem repetição. O helper foi recompilado e passou fixtures próprias de ataque, liberação sem ataque e timeout sem chamada, com restauração de contexto/pilha. Esses testes usam processos de teste próprios, sem abrir o Antica. O novo ataque em outro monstro ainda precisa de validação ao vivo no jogo.

## Follow

A versão 4 inclui Follow (perseguir o alvo), ligado por padrão, aplicado antes de cada novo ataque. Desmarcado aplica o modo de ficar parado no próximo ataque. Veja FOLLOW.md. O Targeting anterior foi confirmado funcionando por você; a nova etapa Follow ainda precisa de teste ao vivo.
