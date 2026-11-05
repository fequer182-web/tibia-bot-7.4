# External Port Notes

A V2.7 original executava dentro do processo e podia validar ABI de Look por breakpoint temporario. O port externo deliberadamente separa:

- **runtime data discovery**, feito por `ReadProcessMemory`;
- **static function discovery**, feito lendo a `.text` do processo e aplicando assinaturas dos profiles de referencia;
- **active ABI validation**, que fica pendente sem instrumentacao interna.

O JSON distingue `confirmed_external_*`, `candidate_static_external` e `pending_external_read_only` para nao promover evidencia mais forte do que realmente foi observada.
