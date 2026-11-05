# PID Injector

Fonte canônico:

`tools/injector/PID_Injector.cpp`

Este é o **único source padrão** do injector. A arquitetura final depende da compilação:

- build x64 -> injector para processos x64;
- build x86 -> injector para processos x86.

O source detecta a arquitetura do alvo e recusa mismatch entre injector e processo. Ele usa o fluxo normal `LoadLibraryW` + `CreateRemoteThread` e não implementa bypass/stealth.

## Binário x64 atual

`x64/PID_Injector_x64.exe`

O mesmo binário também aparece dentro do snapshot histórico:

`reference/x64/ABIBuilder_V1_3/PID_Injector.exe`

## Ramonia x86

O binário x86 atualmente usado nos testes do Ramonia permanece em:

`research/ramonia_x86/tools/injector/PID_Injector_x86.exe`

A partir desta consolidação, novas versões x86/x64 devem ser compiladas a partir do source canônico acima, evitando forks diferentes do injector.
