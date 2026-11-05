"""Backend Ancientia DX: identificacao e diagnostico; nenhuma acao de jogo."""
import hashlib
import struct
from pathlib import Path

from cromin.readers.build import parse_pe, text_section

from cromin.readers.ancientia_profile import DX


def validate_identity(path, disk, loaded):
    if Path(path).name.casefold() != DX['executable'].casefold():
        raise ValueError('Selecione o PID do AncientiaDX.exe, nao o launcher ou o Antica.')
    if hashlib.sha256(disk).hexdigest() != DX['sha256']:
        raise ValueError('AncientiaDX com hash desconhecido. Atualize a analise antes de continuar.')
    for key in ('machine', 'timestamp', 'size_image'):
        if loaded[key] != DX[key]:
            raise ValueError(f'Identidade da memoria divergente: {key}')


def validate_target(memory, path, base, size, pe):
    disk = Path(path).read_bytes()
    validate_identity(path, disk, pe)
    if size != pe['size_image'] or base < 0x10000 or base + size > 0x100000000:
        raise ValueError('Modulo fora dos limites x86 ou tamanho divergente.')
    return dict(profile='ancientia-dx-static-v1', sha256=DX['sha256'],
                architecture='x86', pointerSize=4, actionsEnabled=False,
                offsetsValidated=False)


def read_pointer(memory, address):
    if not 0x10000 <= address <= 0xFFFFFFFC:
        raise ValueError('Endereco de ponteiro fora dos limites x86')
    return struct.unpack('<I', memory.exact(address, 4))[0]


def expected_relocated_text(disk, pe, loaded_base):
    """Aplica HIGHLOW do loader a uma copia de .text, nunca ao processo.

    Tipos desconhecidos e tabelas malformadas interrompem a comparacao.
    """
    section, size = text_section(pe)
    start, end = section['rva'], section['rva'] + size
    raw = disk[section['pointer']:section['pointer']+section['raw']]
    if len(raw) != section['raw']:
        raise ValueError('Secao .text truncada')
    expected = bytearray(raw[:size] + bytes(max(0, size-len(raw))))
    delta = loaded_base - pe['image_base']
    nt = struct.unpack_from('<I', disk, 60)[0]
    opt = nt + 24
    if struct.unpack_from('<H', disk, opt)[0] != 0x10B:
        raise ValueError('Normalizacao implementada somente para PE32 x86')
    count = struct.unpack_from('<I', disk, opt+92)[0]
    if count <= 5:
        if delta:
            raise ValueError('Imagem deslocada sem diretorio de relocacoes')
        return bytes(expected), 0
    reloc_rva, reloc_size = struct.unpack_from('<II', disk, opt+96+5*8)
    if not reloc_rva or not reloc_size:
        if delta:
            raise ValueError('Imagem deslocada sem relocacoes')
        return bytes(expected), 0
    def at(rva, amount):
        for s in pe['sections']:
            if s['rva'] <= rva and rva+amount <= s['rva']+s['raw']:
                offset = s['pointer'] + rva-s['rva']
                data = disk[offset:offset+amount]
                if len(data) == amount:
                    return data
        raise ValueError('Tabela de relocacoes fora dos dados do arquivo')
    table = at(reloc_rva, reloc_size)
    cursor = changed = 0
    seen = set()
    while cursor < len(table):
        if len(table)-cursor < 8:
            raise ValueError('Bloco de relocacoes truncado')
        page, block_size = struct.unpack_from('<II', table, cursor)
        if block_size < 8 or block_size % 2 or cursor+block_size > len(table):
            raise ValueError('Tamanho invalido no bloco de relocacoes')
        for pos in range(cursor+8, cursor+block_size, 2):
            value = struct.unpack_from('<H', table, pos)[0]
            kind, rva = value >> 12, page + (value & 0xFFF)
            if not kind:  # IMAGE_REL_BASED_ABSOLUTE: padding
                continue
            if kind != 3:
                raise ValueError(f'Relocacao x86 nao suportada: {kind}')
            if rva+4 > pe['size_image']:
                raise ValueError('Relocacao fora da imagem')
            if start <= rva < end or rva < start < rva+4:
                if not start <= rva <= end-4 or rva in seen:
                    raise ValueError('Relocacao duplicada ou atravessando .text')
                seen.add(rva)
                index = rva-start
                before = struct.unpack_from('<I', expected, index)[0]
                struct.pack_into('<I', expected, index, (before+delta) & 0xFFFFFFFF)
                changed += 1
        cursor += block_size
    return bytes(expected), changed


def diagnose(memory):
    path = memory.executable()
    base, size = memory.main_module(path)
    def read(offset, amount):
        if offset < 0 or offset+amount > size:
            raise ValueError('Leitura fora do modulo principal')
        return memory.exact(base+offset, amount)
    loaded = parse_pe(read)
    validation = validate_target(memory, path, base, size, loaded)
    disk = Path(path).read_bytes()
    disk_pe = parse_pe(lambda off, amount: disk[off:off+amount])
    if disk_pe['sections'] != loaded['sections']:
        raise ValueError('Layout de secoes diferente entre disco e memoria')
    section, text_size = text_section(loaded)
    first = read(section['rva'], text_size)
    second = read(section['rva'], text_size)
    expected, applied = expected_relocated_text(disk, disk_pe, base)
    stable = first == second
    residual = sum(a != b for a, b in zip(expected, first))
    return dict(schema='cromin-ancientia-diagnostic-v1', pid=memory.pid,
                executable=path, moduleBase=hex(base), aslrDelta=hex(base-disk_pe['image_base']),
                **validation, textSize=text_size, textRva=hex(section['rva']),
                textSha256=hashlib.sha256(first).hexdigest(), stable=stable,
                relocationEntriesApplied=applied, residualDifferentBytes=residual,
                normalizedTextMatches=residual == 0,
                status='identity_and_stable_text' if stable else 'unstable_text',
                note='Diferencas residuais nao identificam sua causa; nenhum offset foi validado.')
