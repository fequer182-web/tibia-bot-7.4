# Desenvolvimento

## Requisitos

Windows, Python x64 e dependências em requirements.txt. Tkinter vem na instalação comum do Python para Windows. A UI não depende de framework instalado dentro do Antica.

## Testes sem conectar ao jogo

Na raiz do projeto:

```powershell
python .\tools\testar.py
```

O runner executa testes do controlador, ciclo/seleção/perseguição, recuperação de alvo/tile, luz e expandir/recolher a interface. Usa memória simulada e, no teste de escrita de luz, uma página alocada no próprio processo de teste. Não abre o processo do Antica. O self-test mostra brevemente o painel de prévia para medir as dimensões, exercita as seções e encerra automaticamente.

Os testes não confirmam o efeito visual da luz nem o funcionamento de novos offsets em uma build diferente. Native code não é revalidado ao vivo por estes testes Python.

## Diagnósticos individuais

Os comandos abaixo conectam ao jogo; não fazem parte do runner local:

```powershell
# Leitura das bags; use o PID atual
python .\tools\bag_read.py --pid PID

# Um ciclo completo da base atual: ataque, abrir corpo e moedas
python .\tools\rat_cycle_once.py --pid PID --thread THREAD --destination 0

# Uma transferência de moeda 3031 entre containers já abertos
python .\tools\loot_once.py --pid PID --thread THREAD --source 1 --destination 0 --count 1
```

Não use os comandos de ação ao mesmo tempo que o painel. Eles são mantidos para diagnóstico; o mutex de painel impede dois painéis, não é um bloqueio global de todos os scripts manuais.

## Compilar C++

Os binários estão em native/bin. O helper de ataque foi recompilado para Targeting; os outros quatro permanecem iguais à V3. CMakeLists.txt permite recompilá-los com toolchain Windows x64. Exemplo com MSYS2 UCRT64:

```powershell
cmake -S .\native -B .\build\native -G Ninja -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe
cmake --build .\build\native
```

O CMake coloca novos binários em build/native/bin. Para usar uma recompilação, feche o painel e copie os seis executáveis dessa pasta para native/bin, após os testes apropriados. A compilação por CMake desta entrega não foi executada no ambiente; os fontes/binários vieram da base anterior. Não substitua binários em uso.

Os helpers também possuem --self-test com fixtures próprias de depuração. São testes separados dos testes Python. Consulte o fonte de cada helper antes de executá-los; não os confunda com prova de compatibilidade em uma build nova do jogo.

## Antes de alterar comportamento

1. Localize os arquivos no mapa docs/ESTRUTURA.md.
2. Reproduza o caso em teste sem o jogo.
3. Altere Python e C++ quando ambos verificarem o mesmo ID/offset.
4. Execute os testes afetados e o runner local.
5. Faça um teste ao vivo de chamada única, registre o JSON e só depois habilite repetição.

Não adicione lógica de memória em ui/panel.py. Não transforme todo ValueError em erro ignorado: SkipCycle é reservado aos casos de evidência temporariamente indisponível. Não mate helpers em andamento para cumprir timeout; a restauração da thread precisa terminar.

Observador Look: native/src/look_watch.cpp → native/bin/OTC_Look_Watch.exe. --self-test cria um processo de teste próprio que executa LookFixture; não é o cliente Antica. Os testes Python usam processos/eventos simulados para conferir a arbitragem. Veja LOOK_MANUAL.md.
