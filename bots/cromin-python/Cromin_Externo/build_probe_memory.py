"""Cromin_py V2: leitura externa de PE e .text; Windows/Python 64 bits."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import os
from pathlib import Path
import struct
import sys
import time

import psutil


def fnv1a64(data):
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"0x{value:016X}"


def hashes(label, data):
    print(f"{label} ({len(data)} bytes)")
    print(f"  FNV1A64 : {fnv1a64(data)}")
    print(f"  SHA256  : {hashlib.sha256(data).hexdigest()}")


def parse_pe(read):
    dos = read(0, 64)
    if dos[:2] != b'MZ':
        raise ValueError('Assinatura MZ ausente')
    offset = struct.unpack_from('<I', dos, 60)[0]
    if not 64 <= offset <= 1024 * 1024:
        raise ValueError('e_lfanew fora do limite')
    coff = read(offset, 24)
    if coff[:4] != b'PE\0\0':
        raise ValueError('Assinatura PE ausente')
    machine, count, timestamp = struct.unpack_from('<HHI', coff, 4)
    optional_size = struct.unpack_from('<H', coff, 20)[0]
    if not 1 <= count <= 96 or not 64 <= optional_size <= 4096:
        raise ValueError('Tabela PE fora dos limites')
    optional = read(offset + 24, optional_size)
    magic = struct.unpack_from('<H', optional)[0]
    if magic not in (0x10B, 0x20B):
        raise ValueError('OptionalHeader desconhecido')
    image_base = struct.unpack_from('<Q' if magic == 0x20B else '<I', optional,
                                    24 if magic == 0x20B else 28)[0]
    size_image = struct.unpack_from('<I', optional, 56)[0]
    sections = []
    table = read(offset + 24 + optional_size, count * 40)
    for index in range(count):
        pos = index * 40
        name = table[pos:pos + 8].rstrip(b'\0').decode('ascii', errors='replace')
        virtual, rva, raw, pointer = struct.unpack_from('<IIII', table, pos + 8)
        sections.append(dict(name=name, virtual=virtual, rva=rva, raw=raw, pointer=pointer))
    return dict(machine=machine, timestamp=timestamp, size_image=size_image,
                image_base=image_base, sections=sections)


def text_section(pe):
    matches = [s for s in pe['sections'] if s['name'] == '.text']
    if len(matches) != 1:
        raise ValueError('Esperada exatamente uma secao .text')
    section = matches[0]
    size = section['virtual'] or section['raw']
    if not 0 < size <= 256 * 1024 * 1024 or section['rva'] + size > pe['size_image']:
        raise ValueError('.text fora dos limites da imagem')
    return section, size


class MODULEENTRY32W(C.Structure):
    _fields_ = [('dwSize', W.DWORD), ('th32ModuleID', W.DWORD),
                ('th32ProcessID', W.DWORD), ('GlblcntUsage', W.DWORD),
                ('ProccntUsage', W.DWORD), ('modBaseAddr', C.c_void_p),
                ('modBaseSize', W.DWORD), ('hModule', W.HMODULE),
                ('szModule', W.WCHAR * 256), ('szExePath', W.WCHAR * 260)]


class Reader:
    def __init__(self, pid):
        self.api = C.WinDLL('kernel32', use_last_error=True)
        signatures = {
            'OpenProcess': ([W.DWORD, W.BOOL, W.DWORD], W.HANDLE),
            'CloseHandle': ([W.HANDLE], W.BOOL),
            'ReadProcessMemory': ([W.HANDLE, C.c_void_p, C.c_void_p,
                                   C.c_size_t, C.POINTER(C.c_size_t)], W.BOOL),
            'CreateToolhelp32Snapshot': ([W.DWORD, W.DWORD], W.HANDLE),
            'Module32FirstW': ([W.HANDLE, C.POINTER(MODULEENTRY32W)], W.BOOL),
            'Module32NextW': ([W.HANDLE, C.POINTER(MODULEENTRY32W)], W.BOOL),
            'QueryFullProcessImageNameW': ([W.HANDLE, W.DWORD, W.LPWSTR,
                                           C.POINTER(W.DWORD)], W.BOOL),
        }
        for name, (args, result) in signatures.items():
            function = getattr(self.api, name)
            function.argtypes, function.restype = args, result
        # Apenas PROCESS_QUERY_INFORMATION | PROCESS_VM_READ.
        self.handle = self.api.OpenProcess(0x0400 | 0x0010, False, pid)
        if not self.handle:
            raise C.WinError(C.get_last_error())
        self.pid = pid

    def close(self):
        if self.handle:
            self.api.CloseHandle(self.handle)
            self.handle = None

    def executable(self):
        buffer = C.create_unicode_buffer(32768)
        size = W.DWORD(len(buffer))
        if not self.api.QueryFullProcessImageNameW(self.handle, 0, buffer, C.byref(size)):
            raise C.WinError(C.get_last_error())
        return buffer.value

    def main_module(self, path):
        for _ in range(5):
            snapshot = self.api.CreateToolhelp32Snapshot(0x08 | 0x10, self.pid)
            if snapshot != C.c_void_p(-1).value:
                break
            error = C.get_last_error()
            if error != 24:
                raise C.WinError(error)
            time.sleep(0.05)
        else:
            raise RuntimeError('Lista de modulos instavel; tente novamente')
        try:
            entry = MODULEENTRY32W()
            entry.dwSize = C.sizeof(entry)
            ok = self.api.Module32FirstW(snapshot, C.byref(entry))
            while ok:
                if os.path.normcase(entry.szExePath) == os.path.normcase(path):
                    return entry.modBaseAddr, entry.modBaseSize
                ok = self.api.Module32NextW(snapshot, C.byref(entry))
            error = C.get_last_error()
            if error != 18:
                raise C.WinError(error)
            raise RuntimeError('Modulo principal nao encontrado pelo caminho do executavel')
        finally:
            self.api.CloseHandle(snapshot)

    def read(self, address, size):
        chunks = []
        while size:
            # Nao atravessa paginas em uma chamada; nunca preenche falhas com zeros.
            amount = min(size, 4096 - address % 4096)
            buffer = C.create_string_buffer(amount)
            received = C.c_size_t()
            ok = self.api.ReadProcessMemory(self.handle, address, buffer, amount,
                                            C.byref(received))
            error = C.get_last_error()
            if not ok or received.value != amount:
                raise RuntimeError(f'Leitura incompleta em 0x{address:X}: '
                                   f'{received.value}/{amount} bytes; WinError {error}. '
                                   'Hash cancelado; nenhuma pagina foi alterada.')
            chunks.append(buffer.raw)
            address += amount
            size -= amount
        return b''.join(chunks)


def probe(pid):
    reader = Reader(pid)
    try:
        path = reader.executable()
        base, module_size = reader.main_module(path)
        print(f'PID          : {pid}\nExecutable   : {path}\nLoaded base  : 0x{base:X}')

        def memory_read(offset, size):
            if offset < 0 or offset + size > module_size:
                raise ValueError('Leitura fora do modulo principal')
            return reader.read(base + offset, size)

        memory_pe = parse_pe(memory_read)
        disk = Path(path).read_bytes()

        def disk_read(offset, size):
            data = disk[offset:offset + size]
            if len(data) != size:
                raise ValueError('Arquivo PE truncado')
            return data

        disk_pe = parse_pe(disk_read)
        print(f'File SHA256  : {hashlib.sha256(disk).hexdigest()}')
        for key in ('machine', 'timestamp', 'size_image', 'image_base'):
            a, b = disk_pe[key], memory_pe[key]
            print(f'{key:12} : disco=0x{a:X} memoria=0x{b:X} '
                  f'{"IGUAL" if a == b else "DIFERENTE"}')
        print(f'ASLR delta   : {base - disk_pe["image_base"]:+#x} (base carregada - base preferida no disco)')
        ds, disk_size = text_section(disk_pe)
        ms, memory_size = text_section(memory_pe)
        print(f'.text memoria: RVA=0x{ms["rva"]:X} VirtualSize=0x{ms["virtual"]:X} '
              f'RawSize=0x{ms["raw"]:X}')
        raw = disk_read(ds['pointer'], ds['raw'])
        hashes('DISCO .text raw (referencia V1)', raw)
        loaded = memory_read(ms['rva'], memory_size)
        hashes('MEMORIA .text VirtualSize (ou RawSize se zero)', loaded)
        # Mesma extensao usada por ResolveBuild no ExternalPort C++.
        cpp_size = min(max(ms['virtual'], ms['raw']), module_size - ms['rva'],
                       memory_pe['size_image'] - ms['rva'])
        if not 0 < cpp_size <= 256 * 1024 * 1024:
            raise ValueError('Extensao .text C++ fora dos limites')
        cpp_loaded = (loaded if cpp_size == memory_size
                      else memory_read(ms['rva'], cpp_size))
        hashes('MEMORIA .text compativel com ExternalPort C++ (max VirtualSize/RawSize)',
               cpp_loaded)
        second = memory_read(ms['rva'], memory_size)
        print('Duas leituras:', 'IGUAIS' if loaded == second else 'INSTAVEL; repita o teste')
        if any(disk_pe[k] != memory_pe[k] for k in ('machine', 'timestamp', 'size_image')) or ds != ms:
            print('Comparacao de bytes cancelada: cabecalhos/layout diferentes.')
            return 2
        # Mesma extensao virtual nos dois lados: corta padding ou acrescenta cauda zero.
        expected = raw[:disk_size] + bytes(max(0, disk_size - len(raw)))
        hashes('DISCO .text em extensao virtual (sem aplicar relocacoes)', expected)
        differences = sum(a != b for a, b in zip(expected, loaded))
        print(f'Comparacao equivalente: {"IGUAL" if expected == loaded else "DIFERENTE"}')
        print(f'Bytes diferentes: {differences}')
        if differences:
            print('Diferenca pode vir de relocacoes do loader ou alteracoes em execucao; '
                  'nao prova adulteracao. Este V2 nao normaliza relocacoes.')
        return 0 if loaded == second else 2
    finally:
        reader.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('process_name', nargs='*', help='Nome exato do processo')
    parser.add_argument('--pid', type=int, help='Selecionar PID explicitamente')
    parser.add_argument('--self-test', action='store_true', help='Ler o proprio Python')
    args = parser.parse_args()
    if sys.platform != 'win32' or C.sizeof(C.c_void_p) != 8:
        parser.error('Use Windows com Python de 64 bits')
    try:
        if args.self_test:
            pid = os.getpid()
        elif args.pid is not None:
            if args.pid <= 0:
                parser.error('PID deve ser positivo')
            pid = args.pid
        else:
            name = ' '.join(args.process_name) or 'Antica Retro opengl.exe'
            matches = []
            for process in psutil.process_iter(['pid', 'name']):
                try:
                    if (process.info['name'] or '').casefold() == name.casefold():
                        matches.append(process.info['pid'])
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    continue
            if len(matches) != 1:
                raise RuntimeError(f'Encontrados {len(matches)} processos: {matches}. '
                                   'Abra o programa ou selecione --pid NUMERO.')
            pid = matches[0]
        return probe(pid)
    except (OSError, RuntimeError, ValueError, psutil.Error) as error:
        print(f'[ERRO] {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
