# Como localizar processos, builds e campos

## 1. Processo e executável

Abra o cliente pelo launcher e entre no personagem. Para listar o processo DX, use:

```powershell
Get-Process AncientiaDX | Select-Object Id, MainWindowTitle
```

Use o PID que corresponde à janela desejada. Repita após reiniciar o cliente.

## 2. Arquitetura, hash e base carregada

Da raiz do repositório:

```powershell
python pesquisa/BuildProbe_static.py "CAMINHO_DO_CLIENTE" --out inventario.json
python ferramentas/diagnostico-python/build_probe_memory.py --pid 12345
```

Troque `12345` pelo PID real. O primeiro comando inventaria arquivos PE e imports em disco. O segundo mede a build carregada e compara a seção de código. Compare arquitetura, tamanho da imagem, timestamp, hash e base. Endereço absoluto depende da base carregada. Diferença entre a seção em memória e em disco pode vir de relocação.

## 3. Valores do jogo

```powershell
python ferramentas/diagnostico-python/value_scan.py --pid 12345 --field hp
python pesquisa/ancientia_build_nova.py --pids 12345
python pesquisa/ancientia_player.py --pid 12345 --sem-seed
python ancientia_loot.py --pid 12345
```

Na busca de HP, comece com um valor conhecido e refine após mudanças observáveis. Um candidato único em uma captura ainda precisa repetir o resultado. No Ancientia, confira nome e posição junto com vida e mana. Para inventário, compare a bag aberta e um corpo aberto. A lista usa IDs dos recipientes e slots, e o subtipo bruto do item depende da classe.

## 4. Builders, referências e pipe

| Método | Onde está |
| --- | --- |
| Busca guiada e exportação de perfil | [ExternalBuilderGuided](../ferramentas/ExternalBuilderGuided) e [ExternalBuilderLevels](../ferramentas/ExternalBuilderLevels) |
| Exploração de build x64, rebind de jogador e ABI | [BuildExplorer e ABIBuilder](../bots/crome-cpp/reference/x64) |
| Observação de caminhada, destino e chamadas | [OTC Walk Trace](../ferramentas/otc-walk-trace) |
| Ramonia x86 e teste do contrato da pipe | [ResearchProbe e bridge](../bots/crome-cpp/research/ramonia_x86) |
| Calibração e contrato JSON de OTClient | [OTC API](../ferramentas/otc-api) e [documentação da API](../ferramentas/otc-api/docs/API.md) |

Associe o perfil a um hash e uma arquitetura antes de usar qualquer offset. Para projetos com bridge, siga a sequência de inventário, validação de leitura e teste da pipe descrita no projeto correspondente. Os perfis históricos de x86 e x64 são específicos das builds que os produziram. O compacto Ancientia usa seu leitor externo.
