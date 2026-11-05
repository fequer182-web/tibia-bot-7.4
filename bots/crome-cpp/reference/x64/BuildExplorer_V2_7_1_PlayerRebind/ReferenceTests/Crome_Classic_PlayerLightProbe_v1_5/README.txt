CROME CLASSIC PLAYER LIGHT PROBE v1.5
====================================

Build alvo:
  Timestamp: 0x6AA3019A
  ImageSize: 0x14C1000
  Hash/profile: 602E746BAE706D6C

Objetivo:
  Descobrir o byte (ou par de bytes intensity/color) da luz propria do LocalPlayer
  usando estados reais: sem luz -> utevo lux -> utevo gran lux -> sem luz.

Este probe e SOMENTE LEITURA. Ele nao escreve memoria.

Base do JSON:
  LocalPlayer direct   RVA 0x13EF528
  LocalPlayer indirect RVA 0x13EF550, owner +0x1F8
  LocalPlayer vtable   RVA 0xFBFD18
  X +0x30, Y +0x34, Z +0x38, Direction +0x7C

Como usar:
  1. Abra o Classico e logue o personagem.
  2. Rode build.bat.
  3. Rode run_test.bat.
  4. Sem magia de luz: F5.
  5. Fale "utevo lux"; enquanto estiver ativo: F6.
  6. Fale "utevo gran lux"; enquanto estiver ativo: F7.
  7. Espere a luz voltar ao normal; F8 (opcional, mas recomendado).
  8. F9 imprime/rankeia candidatos u8 e pares de bytes vizinhos.
  9. Envie o bloco [ANALYZE] e [PAIRS].

F10 revalida o LocalPlayer e mostra status.
END descarrega o probe.
