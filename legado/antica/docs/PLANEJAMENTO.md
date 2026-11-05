# Planejamento das próximas versões

## O que esta entrega muda

Organização do código em pacotes, caminhos independentes da pasta aberta no PowerShell e painel recolhível. Mantém as ações da base V3, incluindo ignorar ciclos sem evidência suficiente do tile/corpo. Não implementa os recursos futuros apenas por adicionar seus nomes à interface.

## Situação dos pedidos

| Recurso | Agora | Próxima entrega necessária |
| --- | --- | --- |
| Painel pequeno com abas | Modelo do Crome: 260x100 recolhida, seis seções em 260x290, botão + para 520x460 e rolagem. | Ajustar o visual com sua avaliação no monitor. |
| Battle por nomes | Implementado com testes locais: lista de nomes completos, comparação sem diferença de maiúsculas e conferência no helper. | Validar ao vivo com outro monstro além de Rat. |
| Qualquer criatura | Qualquer **Monster** válido implementado. NPCs e jogadores excluídos. | Validar ao vivo em diferentes monstros da build conhecida. |
| Loot por IDs | V8: corpos, itens de loot e Drop por listas; Drop somente do corpo no SQM atual. | Validar ao vivo novos IDs; ampliar metadados de empilhamento. V9 já abre bags 2853 aninhadas com limites. |
| Look para IDs | V6: observa o Look normal e mostra o ID do item ao lado do PID, sem aba. Testes locais passaram. | Validar no Antica e coletar IDs dos corpos/itens para generalizar loot. |
| Cavebot com rope/shovel/use | Só caminhada. | Tipos de waypoint e uma ação validada de uso por vez. |
| Ferramentas com IDs do Olders | ZIP do Crome forneceu a referência de pesca (vara 3483, água 4597–4602); falta validar no Antica. IDs de rope/shovel ainda pendentes. | Importar a configuração correta do Olders e conferir os itens no Antica com Look/leitor. |
| Pesca | Não implementada. | Identificar vara, água e uso da vara no tile, depois intervalo/limite/parada. |
| Light hack | Campo experimental de intensidade já existe. | Confirmar o efeito visual; se não funcionar, pesquisar o campo correto antes de ampliar. |
| UI injetada | Não disponível; DLL anterior foi detectada. | Investigação separada. Esta base continua externa, sem injetor ou mecanismo para ocultar DLL. |

Targeting desta entrega: veja TARGETING.md. O loot de outros monstros permanece separado.

## Ordem recomendada de implementação

### 1. Look e identificação de itens

Ler bag e tile escolhido em modo somente leitura. Mostrar ID, nome quando houver fonte validada, posição, container e slot. Permitir copiar o ID para configuração de loot/ferramentas. Não interpretar count/subtype como quantidade sem conhecer o tipo do item.

Pronto quando: itens de referência conferem com os IDs observados, tile ausente não interrompe o painel, e copiar o ID não executa uma ação.

### 2. Battle configurável

Adicionar lista de nomes, opção qualquer monstro, exclusões e alcance no mesmo andar. Decidir regras de prioridade (mais próximo inicialmente). Generalizar leitores e helper, mantendo conferência de identidade, tipo Monster, vida e referência própria. O filtro da UI não pode ser contornado por um helper ainda fixo em Rat.

Pronto quando: dois nomes diferentes são selecionados corretamente, exclusões funcionam, criaturas inválidas não entram e existe uma chamada de ataque por alvo.

### 3. Loot por IDs

Separar IDs de corpos/containers dos IDs dos itens. Registrar por item se é empilhável e como verificar a quantidade. Primeira versão pode transferir um item por vez com confirmação; o modo contínuo vem depois. Nunca assumir que todos os itens usam a mesma semântica da moeda.

Pronto quando: item permitido é recolhido, item não permitido permanece, container/slot alterado cancela a transferência e a quantidade recebida confere.

### 4. Waypoints com ações e ferramentas

Evoluir o modelo Position para Waypoint, com `kind`, coordenadas e `tool_id` quando necessário. Tipos pretendidos: walk, use, rope, shovel e wait. Migrar configurações antigas como `kind=walk`, preservando as coordenadas. Validar Game::use/Game::useWith e seus argumentos antes de ligar os novos tipos. As funções ainda não estão validadas na base atual.

Pronto quando: cada ferramenta funciona em chamada única, com item e tile conferidos, e a rota só avança após observar a transição esperada. Falta de ferramenta pausa/pula conforme regra configurada, sem repetir indefinidamente.

### 5. Pesca

Reutilizar a ação validada de useWith. Configurar vara e lista de tiles de água selecionados, intervalo mínimo, máximo de tentativas e condição de parada. Excluir tiles distantes, outro andar ou sem água reconhecida. Não iniciar automaticamente ao conectar.

Pronto quando: um uso de vara é observado corretamente antes de permitir repetição.

## Regras de atualização

- Identificar o recurso como planejado, experimental ou validado; não apresentar botão ativo para operação inexistente.
- Salvar configurações por perfil de build/servidor depois da centralização dos offsets.
- Registrar o motivo de ciclo ignorado, além das confirmações de sucesso.
- Manter separação entre falta de evidência do mundo e falha de restauração de uma chamada.
- Testar primeiro com memória simulada/fixtures; depois chamada única e teste ao vivo; só então integrar ao funcionamento contínuo.

## Dívidas técnicas conhecidas

Offsets e checksums ainda aparecem em mais de um arquivo, especialmente C++. Os nomes internos combat_cycle/rat no código herdado ainda refletem o teste original. O scanner memory.py também contém CLI antigo além da classe de leitura. Os cinco executáveis repetem utilidades por inclusão de trace_walk.cpp. Centralizar essas peças facilita manutenção, mas exige regressões antes de substituir a base testada.

O perfil Antica anterior informa a build 0x6AD91836 / SizeOfImage 0xEDE000 / .text FNV1a64 0xBF68517610291E3A. Os binários continuam recusando build divergente. Uma atualização do cliente exige nova verificação; caminhos e base ASLR da sessão anterior não são um perfil reutilizável.
