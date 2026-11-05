# Interface compacta

O Crome inicia recolhido, com 260 x 100 pixels e fonte 10. Os seis botões
abrem o respectivo painel abaixo do menu; clicar novamente recolhe o painel.
A janela fica acima das outras janelas por padrão e pode ser arrastada pelo título.

Controles da barra:

- **O**: carregar profile.
- **S**: salvar profile.
- **P**: fixar/desafixar a janela acima do jogo.
- **+**: ampliar/reduzir o painel para editar formulários maiores.
- **D**: alternar tema claro/escuro.
- **-**: minimizar.
- **X**: fechar.

Healing contém as abas Healing, Party e Rune. Tools contém Tools, Alarmes e
Runtime; o botão IDs abre o mesmo catálogo usado no Cavebot. Targeting usa
os controles existentes de combate, em um painel próprio.

As funções de desenho antigas também mantêm configuração e gravação de
waypoints. A UI continua executando essas atualizações numa área sem
entrada e fora da tela, mesmo com os editores fechados. Os controles de
disponibilidade do backend continuam vigentes.

Os formulários mais largos têm rolagem horizontal. O botão + oferece uma
área maior para edição. Popups de edição ampliam temporariamente a janela.

## Conferência visual sem cliente

O executável aceita `--preview=compact`, `healing`, `party`, `rune`,
`cavebot`, `targeting`, `tools`, `alarms`, `loot`, `hotkeys` ou
`runtime` ou `ids`. `--wide` permite conferir a largura ampliada. Exemplo:

```text
Crome.exe --preview=healing
```

Esse modo desenha cinco frames em uma janela oculta, grava
`preview-healing.bmp` ao lado do executável e encerra. Não inicializa a
sessão, o carregamento do runtime ou o worker Ramonia.

O build Release x64 do executável e do runtime foi conferido. Quatorze telas
foram renderizadas com sucesso. A conexão com o cliente e o comportamento
dos módulos em jogo continuam dependendo da validação real.
