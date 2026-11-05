"""Inventario PE estatico. Python 3, sem dependencias e sem executar o cliente.
Uso: python BuildProbe_static.py PASTA --out inventario.json
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import struct


def inspect(path):
    b = path.read_bytes()
    def u(fmt, off):
        return struct.unpack_from('<' + fmt, b, off)[0]
    def string(off):
        end = b.find(b'\0', off)
        if end < 0:
            raise ValueError('String sem terminador')
        return b[off:end].decode('ascii', 'replace')
    if b[:2] != b'MZ':
        raise ValueError('MZ ausente')
    pe = u('I', 60)
    if b[pe:pe+4] != b'PE\0\0':
        raise ValueError('PE ausente')
    count, optsize = u('H', pe+6), u('H', pe+20)
    opt = pe+24
    magic = u('H', opt)
    if magic not in (0x10b, 0x20b) or not 1 <= count <= 96:
        raise ValueError('Cabecalho nao suportado')
    x64 = magic == 0x20b
    sections = []
    for i in range(count):
        p = opt+optsize+i*40
        vs, rva, size, raw = struct.unpack_from('<IIII', b, p+8)
        data = b[raw:raw+size]
        sections.append(dict(name=b[p:p+8].rstrip(b'\0').decode('ascii','replace'),
                             rva=rva, virtualSize=vs, rawSize=size, rawOffset=raw,
                             sha256Raw=hashlib.sha256(data).hexdigest()))
    def offset(rva):
        if rva < u('I', opt+60):
            return rva
        for s in sections:
            if s['rva'] <= rva < s['rva']+s['rawSize']:
                return s['rawOffset']+rva-s['rva']
        raise ValueError(f'RVA sem dados em disco: {rva:x}')
    directories = opt+(112 if x64 else 96)
    ndir = u('I', opt+(108 if x64 else 92))
    def directory(index):
        return struct.unpack_from('<II', b, directories+index*8) if index < ndir else (0,0)
    imports = []
    imp, imp_size = directory(1)
    if imp:
        p = offset(imp)
        for _ in range(min(4096, imp_size//20)):
            row = struct.unpack_from('<IIIII', b, p)
            p += 20
            if not any(row):
                break
            thunk, _, _, name, first = row
            names = []
            t = offset(thunk or first)
            width = 8 if x64 else 4
            for j in range(65536):
                v = u('Q' if x64 else 'I', t+j*width)
                if not v:
                    break
                names.append(f'ordinal:{v & 65535}' if v & (1 << (width*8-1)) else string(offset(v)+2))
            imports.append(dict(dll=string(offset(name)), symbols=names))
    exports = []
    exp, _ = directory(0)
    if exp:
        e = offset(exp)
        names, table = u('I', e+24), u('I', e+32)
        for i in range(min(names,65536)):
            exports.append(string(offset(u('I', offset(table)+i*4))))
    delay_rva, delay_size = directory(13)
    return dict(file=path.name, size=len(b), sha256=hashlib.sha256(b).hexdigest(),
                machine=hex(u('H',pe+4)), architecture={0x14c:'x86',0x8664:'x64'}.get(u('H',pe+4),'other'),
                timestamp=hex(u('I',pe+8)), entryPointRva=hex(u('I',opt+16)),
                imageBase=hex(u('Q' if x64 else 'I',opt+(24 if x64 else 28))),
                sizeOfImage=hex(u('I',opt+56)), dllCharacteristics=hex(u('H',opt+70)),
                sections=sections, imports=imports, exports=exports,
                delayImportDirectory=dict(rva=hex(delay_rva),size=delay_size))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('folder', type=Path)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    if not args.folder.is_dir():
        p.error('Pasta inexistente')
    files = sorted(x for x in args.folder.rglob('*') if x.is_file())
    results = []
    for f in files:
        if f.suffix.lower() in ('.exe','.dll'):
            try:
                result = inspect(f)
                result['relativePath'] = f.relative_to(args.folder).as_posix()
            except Exception as e:
                result = dict(file=f.name,error=str(e))
            results.append(result)
    scripts = collections.Counter()
    for f in files:
        if f.suffix.lower() in ('.lua','.otmod'):
            with f.open('rb') as stream:
                head = stream.read(4)
            scripts[f.suffix.lower()+':'+('ENC3' if head == b'ENC3' else 'other')] += 1
    report = dict(mode='static_only',executedClient=False,binaries=results,
                  scriptHeaders=dict(scripts), limitations=[
                      'Imports estaticos nao incluem todas as cargas dinamicas.',
                      'Diretorio delay-import registrado, nao enumerado.',
                      'Hashes raw de secao nao equivalem a hashes de memoria.',
                      'Nenhum offset, ABI ou acesso ao processo foi validado.'])
    args.out.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(args.out)


if __name__ == '__main__':
    main()
