CROME CLASSIC FEATURE PROBE v1 - LOOK + LUX
===========================================

BUILD ALVO
- Processo: Classico-1790708814.exe (fallback Classico-*.exe)
- Timestamp: 0x6AA3019A
- ImageSize: 0x14C1000
- Hash: 0x602E746BAE706D6C
- Profile incluido em profile\

IMPORTANTE
- Feche Crome/Rift/qualquer outro probe antes do teste.
- Abra o OT Classico e entre no personagem.
- Rode build.bat uma vez.
- Rode run_test.bat.

LOOK
O profile desta build informa:
- Game::look RVA = 0x164090
- Item ID = Thing + 0x40
- Position = +0x30 / +0x34 / +0x38

O probe instala um hook somente se os 18 bytes iniciais baterem exatamente com a assinatura esperada.
Depois de injetar:
1) Look em um item no chao.
2) Look em um item dentro da backpack.
3) Se puder, Look em ladder/chest do mapa.
Copie os blocos [LOOK #].

LUX / FULL LIGHT
F6 = procura accessors x64 de UIMap::get/setMinimumAmbientLight e tenta localizar o UIMap ativo.
F7 = salva o valor atual e escreve 1.0 no minimum ambient light.
F8 = restaura o valor original salvo.

O scanner imprime:
- getter RVA
- setter RVA
- UIMap viewOffset
- MapView lightOffset
- endereco do UIMap / MapView
- valor atual

Se F7 clarear o mapa e F8 voltar ao normal, a descoberta foi validada visualmente.

TECLAS
F6  descobrir/vincular Light
F7  Full Light ON = 1.0
F8  restaurar Light
F9  dump Game::look
END restaurar tudo e descarregar DLL

SEGURANCA DO TESTE
- Look: se a assinatura nao bater, nao faz patch parcial.
- Light: o valor anterior e salvo antes de alterar e restaurado no F8/END.
- Este pacote e probe isolado; nao altera o Crome principal nem o BuildProfile.

MANDE O LOG
Envie:
1) blocos [LOOK #] do chao e backpack;
2) toda a secao [LIGHT] apos F6;
3) diga se F7 clareou e se F8 restaurou.
