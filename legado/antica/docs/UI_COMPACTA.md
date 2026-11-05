# Interface compacta no formato do Crome

A referência foi o ZIP Crome_UI_Compacta fornecido por você: screenshots de compact, targeting, healing e tools, e o documento UI_COMPACTA.md. Os executáveis e DLLs do ZIP não foram executados nem incorporados ao painel.

## Comportamento

- Abre recolhido em **260x100**.
- Seis botões em duas linhas: **Healing, Cavebot, Targeting, Loot, Hotkeys e Tools**.
- Clicar numa seção abre o formulário abaixo do menu, em **260x290** (Tools usa **260x190**).
- Clicar novamente na seção selecionada recolhe. Trocar de seção mantém o painel aberto.
- Barra horizontal e vertical permitem editar formulários maiores.
- **+** alterna a edição ampla, de **520x460** quando aberto.
- O título pode ser arrastado. O painel fica acima das outras janelas por padrão.
- **Aplicar** e **Parar** ficam abaixo do formulário aberto. F8 também para, quando o painel tem foco.

| Botão | Função |
| --- | --- |
| O | Carregar configuração JSON do Cromin. Exige desconectar primeiro; não liga módulos. |
| S | Salvar a configuração atual e waypoints. Não salva módulos ligados. |
| P | Fixar/desafixar acima das outras janelas. |
| + | Alternar largura normal/ampla. |
| D | Alternar tema claro/escuro. |
| - | Minimizar. |
| X | Fechar, aguardando uma chamada atual retornar. |

Os JSONs de build/ABI do Crome não são configurações válidas para O/S. Não importar endereços/funções de Olders no Antica.

## Conectar

Abra **rodapé → Conectar ao Antica**, na janela Conexão. Confira PID, build e personagem. Use o botão Parar para interromper novas solicitações. Recolher as abas não desativa os módulos.

Healing, Cavebot, Targeting, Loot e Light continuam usando a mesma base anterior. A referência visual não transfere os recursos de pesca, alarmes, party, rune, loot por IDs configuráveis. Targeting por nomes e qualquer monstro já está implementado; veja TARGETING.md. Os demais recursos continuam no roteiro; os formulários descrevem os limites atuais.

## Prévias desta entrega

- preview/compacto.png
- preview/targeting.png
- preview/healing.png
- preview/tools-amplo.png

As imagens foram renderizadas a partir do próprio painel, sem conectar ao Antica. O self-test cobre dimensões, abrir/recolher todas as seções, modo amplo, temas, fixar e estado inicial desligado. Abrir/salvar configurações tem validação local de formato e limites. Falta teste ao vivo da interface atualizada.

## Referência de pesca do Olders

O ZIP fornecido contém rod_id **3483**, water_ids **4597–4602**, intervalo de 850 a 1450 ms e pausa durante targeting/walker. A cópia em referencias/olders_fishing.json serve apenas para planejar a implementação. Esses IDs ainda precisam ser conferidos no Antica; a pesca não é executada por esta UI.

## Simplificação de Tools na versão 4

Tools contém somente Light Hack, sem slider nem textos adicionais. O valor é sempre 31, inclusive ao carregar uma configuração antiga. A conexão e os diagnósticos ficam numa janela separada, aberta clicando no rodapé.
