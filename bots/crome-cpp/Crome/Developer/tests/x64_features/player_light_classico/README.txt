Crome Classic PlayerLight WriteProbe v1.6

Build alvo:
  timestamp 0x6AA3019A
  image size 0x14C1000
  hash/profile 602E746BAE706D6C

Validacao anterior:
  LocalPlayer +0xF1 (u8)
  BASE 0 -> utevo lux 6 -> utevo gran lux 8 -> BASE 0

Este probe testa ESCRITA somente no byte LocalPlayer+0xF1.
Ele salva o valor original e restaura com F9 e tambem ao pressionar END.
Enquanto um teste estiver ativo, o byte e reaplicado a cada 50 ms para evitar que o jogo o sobrescreva durante a validacao.

TECLAS:
  F5  status/resolver LocalPlayer
  F6  forcar 6
  F7  forcar 8
  F8  forcar 15
  F9  restaurar original/parar force
  END restaurar/descarregar

Teste sugerido:
  1) entre em uma area escura / buraco
  2) F5 e anote o Light atual
  3) F6: comparar visualmente com utevo lux
  4) F7: comparar com utevo gran lux
  5) F8: ver se aumenta alem de gran lux
  6) F9: confirmar que volta ao original

Nao usa stealth/evasao; e um probe de validacao isolado para a build informada.
