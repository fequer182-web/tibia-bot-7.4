# Crome

Repositório principal do Crome e das ferramentas de pesquisa/build usadas para manter perfis de builds diferentes.

## Estrutura

- `Crome/` — projeto principal.
  - `Developer/` — código-fonte, CMake, módulos, testes e scripts de build.
  - `Crome/` — distribuição/runtime usada pelo usuário.
- `research/ramonia_x86/` — pesquisa atual da build Ramonia x86 e profile correspondente.
- `reference/x64/` — ferramentas e projetos x64 mantidos como referência técnica.
- `docs/` — estado geral do projeto e notas de organização.

## Build principal

Entre em `Crome/Developer/` e execute `build.bat`.

O layout `Developer/ -> ../Crome/` foi preservado de propósito porque os scripts atuais dependem dessa relação.

## Regra do repositório

Artefatos de build temporários não devem ser versionados. A pasta `build/`, CMake intermediário, objetos e logs estão cobertos pelo `.gitignore`.
