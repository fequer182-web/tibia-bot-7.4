# Crome Internal Bot - prototipo 0.1

CODIGO-FONTE, AINDA SEM EXECUTAVEL. A compilacao do TFS foi tentada e bloqueou em
MSB4019: Build Tools sem componente C++. Nenhum servidor ou banco foi alterado.

O prototipo controla um Player real existente e offline, sem OTC. Anda no mesmo
andar, acompanha/ataca Rat, pega gold/cheese do proprio corpo, come pela action
original do OT, recua e salva/para. Limites: raio10 e dez minutos. A politica e FSM;
nao integra ainda o aprendizado por reforco Python. Nao inclui templo->bueiro,
escadas, quests, lojas, banco ou campanha completa ate8. Primeiro validar executor.

## Preparacao

1. Extraia fora do servidor original.
2. Visual Studio Installer precisa de Desenvolvimento para desktop com C++,
   MSVCv143 e WindowsSDK. Tambem sao necessarias dependencias C++ do TFS (Boost,
   pugixml, fmt, LuaJIT, MySQL/OpenSSL etc.). Nao ha instalacao automatica.
3. PREPARAR_E_COMPILAR.cmd confere hashes e prepara somente codigo numa copia em
   Documents/CromeInternalBotLab/source. A origem fica intacta. Nao sobrescreve
   laboratorio de outra versao.
4. Build concluido produz build/bin/crome-internal-bot.exe. O script nao o inicia.
   Ainda precisa de DLLs, dados e configuracao.
5. Valide em instancia separada com COPIA do banco e dos dados. Nao mantenha duas
   instancias escrevendo no mesmo banco. Instale tambem o Lua em data/scripts.

Guia oficial C++: https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools

## Teste apos compilar

Conta NORMAL, personagem nivel1, login inicial ja feito, arma melee e bag equipadas,
OFFLINE. Nenhum item, ouro ou XP e concedido pelo modulo. Use personagem dedicado:
a posicao e o progresso serao salvos. O modulo recusa GM/god como bot.

Com outro personagem GM, va a uma area aberta apenas com Rats na cave, andar8.
O operador escolhe esse ponto inicial; nao ha sequencia fixa de waypoints.

/cromebot start,NomeDoPersonagem
/cromebot status
/cromebot stop

O bot surge num tile livre junto ao GM. Pare antes de entrar nele pelo OTC.
Morte segue as regras do TFS. Nao ha respawn automatico. Stop/timeout usam save e
logout normais. Eventos no console e data/crome_internal_bot.log; snapshot de HP,
XP, posicao e save a cada30s. Um OTC humano pode observar o jogador, sem controla-lo.

## Contrato

- No maximo um bot, Player normal existente nivel1-7, inicializado e equipado.
- Lease5s renovada pelo controlador; erro/parada do Lua permite logout/save.
  O ping dos jogadores humanos nao muda. Login no Player ativo e bloqueado.
- Passo por startAutoWalk normal, cooldown e posicao observada no proximo tick.
  Nao usa o atalho Lua Creature.move com FLAG_NOLIMIT.
- Loot por internalMoveItem com flags0, ator/eventos; cap e espaco normais.
  Owner do corpse usa runtimeID, conforme Monster::getCorpse deste OT.
- Comida por Actions::useItem, sem duplicar a formula nem cura instantanea.
- Percepcao no mesmo andar e viewport7x5; sem busca global de alvos.

## Validacao e limites

Modulo real testado em Lua5.1 com motor simulado: GM gate, um bot, aproximacao,
ataque, XP/corpo proprio, gold/comida, Snake, recuperacao, save/stop, callback
antigo, passo atrasado e limite de partida. Adaptador passou g++ -fsyntax-only
com stubs das assinaturas do motor. Isto NAO e compilar/linkar o TFS completo.
Preparador, hashes e contratos de fonte tambem testados. O build real bloqueou
por falta do componente C++. Sem teste de mapa/banco reais; sem executavel.

src/ contem cinco arquivos alterados, nao o servidor completo. alteracoes.patch
mostra o diff e manifest.json identifica a origem exata. Cabecalhos GPLv2-ou-
posterior originais preservados; COPYING contem uma copia da GPLv3.
