# Como funciona

1. **BuildProbe** inventaria os binários sem abrir o cliente. Exemplo: `python pesquisa/BuildProbe_static.py "CAMINHO_DO_CLIENTE" --out inventario.json`.
2. **Conexão por PID** valida a arquitetura, o hash e o perfil de AncientiaDX. O leitor usa a base carregada do módulo, pois ela pode mudar entre execuções.
3. **Leitura externa** resolve o jogador e o alvo a partir do perfil. Os recipientes abertos vêm da árvore de containers; os slots são lidos no deque de cada container. Entradas vazias na árvore são ignoradas. O subtipo bruto de um item não deve ser interpretado como quantidade para todos os IDs.
4. **Controle** usa o estado lido para pausar a rota durante combate e durante a tentativa de abrir o corpo. Teclas e cliques são enviados somente ao cliente em foco; o loot é recolhido pelo próprio jogo.
5. **Diagnóstico** registra em `reports/` as ações e o resultado observado. As rotas locais ficam em `data/`, separadas por build e personagem. Revise os dados antes de compartilhar qualquer relatório.

## Integração nativa

O plano original era BuildProbe → CromePipeTest → bridge. Neste repositório, a validação de build é usada pelo leitor externo. CromePipeTest, bridge nativo e execução de Lua não estão integrados ao compacto. O observador C++ em `pesquisa/` é pesquisa incompleta: não faz parte da execução padrão.

## Alteração de build

Não reutilize endereços de Antica x64 em Ancientia x86. Primeiro compare os binários, depois valide os leitores com dados observáveis no jogo. Só habilite ações quando a leitura do mesmo PID e personagem estiver consistente.
