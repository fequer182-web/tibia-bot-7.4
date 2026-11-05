"""Perfil exclusivo da build DX e6c7... obtido no executavel e conferido por leitura."""
DX=dict(executable='AncientiaDX.exe',machine=0x14C,timestamp=0x6AEBB27C,size_image=0xDDF000,
        sha256='e6c7daedcab8c324697ce55c564a7300997507fd44454854109707c344208be2')
GAME_RVA=0xD43090
GLOBAL_RVA=GAME_RVA+0x30
VTABLE_RVA=0x85D974
MONSTER_VTABLE_RVA=0xABA974
LIGHT_TRANSITIONS_VALIDATED=True
LIGHT_WRITE_ALLOWED=True
LIGHT_VISUAL_VERIFIED=False
LIGHT_WRITE_VALIDATED=False  # compatibilidade: nao afirma teste visual/escrita ao vivo
NATIVE_CALLS_VALIDATED=False
READER_SIGNATURES=(
    (0x58CE0+0x2D,'8b4930'),
    (0x5E6D0+0x2D,'8b4934'),
    (0x5E730+0x2D,'8b4938'),
    (0x5BA40,'8a415cc3'),
    (0x5BA60,'8b4160c3'))
