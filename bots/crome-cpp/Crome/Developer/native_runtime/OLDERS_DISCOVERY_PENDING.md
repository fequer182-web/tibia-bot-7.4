# Descobertas pendentes para Olders

## Should Attack Me: foco do monstro

Regra solicitada: quando outro player estiver visivel, adquirir somente monstros
que estejam focando o LocalPlayer. Ignorar os que focam outro player. Sem outro
player visivel, usar o targeting normal. A opcao atual no menu nao filtra alvos.

- Descobrir se o cliente recebe/expoe o alvo real de cada monstro e detectar
  players visiveis, excluindo LocalPlayer. Direcao/animacao nao comprova foco.
- Capturar casos controlados: monstro em mim, monstro em outro player, troca de
  foco, outro player saindo da tela, alvo morto e ausencia de evidencia.
- Registrar campo/metodo, tipos, lifetime, fingerprint e evidencias no profile.
- Automatizar a busca e oferecer testes guiados para novas builds. Reusar o
  procedimento aprendido; cada build valida seus offsets e ABI, sem copiar RVA.
- Ligar o filtro a aquisicao/revalidacao do targeting somente com evidencias.
  Com outro player e foco desconhecido, nao adquirir o monstro.

## Pathable IDs

Integrar a lista a caminhada: cadastrar um ID ainda nao modifica o pathfinding
do LocalPlayer::autoWalk. Descobrir/validar como tratar itens que permitem
passagem sem ignorar paredes, pisos invalidos e bloqueios reais do servidor.

## Pesca: validacao depois das correcoes de concorrencia

O dump de 10/10/2026 (Olders_OpenGL.exe.24144.dmp) registra 0xC0000374.
O retorno do hook no stack corresponde a CallWindowProc, nao prova falha em
Game::useWith. Na baseline, useWith 0x164A50 empresta source/target e nao libera
as referencias dos argumentos no retorno; nao aplicar a correcao de attack ali.

Corrigidos payload de busca alterado antes do claim e filebuf compartilhado sem
mutex. Reobter a vara no game thread antes da busca. Ainda confirmar no cliente
pesca repetida, cliques manuais, mudanca de posicao e coexistencia com modulos.
Estas falhas eram reais no codigo, mas o dump nao fecha a causa unica do crash.
