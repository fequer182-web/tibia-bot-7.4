# Crome v7.12 - teste rapido

1. Compile com `Developer\build.bat`.
2. Abra Olders e Crome.
3. Teste `Save Profile...` e `Load Profile...`: ambos devem abrir o dialogo do Windows.
4. Para Fishing: Cavebot/Targeting OFF, configure Fishing Rod IDs e Fishing Water IDs, ligue Auto Fishing. A DLL recarrega `fishing.json` sem precisar reinjetar.
5. Procure no console `[FISH CFG] reloaded` e depois `[FISH] auto request accepted`.
6. Look Olders continua em Game::detailedLook RVA 0x163B10.
7. Tools sao persistidos em `config/tools.json`; funcoes que dependem de move/equip continuam marcadas como backend pendente ate ABI ser validada.
