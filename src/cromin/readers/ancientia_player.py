"""Descoberta x86 por RTTI/vtable + seed HP + atributos no objeto.

Reaproveita o metodo do RamoniaResearchProbe, nao seus RVAs/offsets.
"""
import math
import struct
import time
from pathlib import Path
from cromin.readers.ancientia import DX, validate_target
from cromin.readers.build import parse_pe
from cromin.readers.memory import blocks


def matches(data, needle):
    pos = data.find(needle)
    while pos >= 0:
        yield pos
        pos = data.find(needle, pos+1)


def resolve_rtti(disk, class_name='LocalPlayer'):
    pe = parse_pe(lambda o,n: disk[o:o+n])
    if pe['machine'] != 0x14c:
        raise ValueError('RTTI implementado para x86')
    base = pe['image_base']
    def rva(offset):
        for s in pe['sections']:
            if s['pointer'] <= offset < s['pointer']+s['raw']:
                return s['rva']+offset-s['pointer']
        return None
    def raw(address, amount):
        for s in pe['sections']:
            if s['rva'] <= address-base and address-base+amount <= s['rva']+s['raw']:
                off = s['pointer']+address-base-s['rva']
                value = disk[off:off+amount]
                if len(value) == amount:
                    return value
        return None
    text = next(s for s in pe['sections'] if s['name'] == '.text')
    def code(address):
        return base+text['rva'] <= address < base+text['rva']+text['virtual']
    found = []
    for name_offset in matches(disk, ('.?AV'+class_name+'@@\0').encode('ascii')):
        td_rva = rva(name_offset-8)
        if td_rva is None:
            continue
        td = base+td_rva
        for ref in matches(disk, struct.pack('<I',td)):
            if ref < 12:
                continue
            col_off = ref-12
            col_rva = rva(col_off)
            if col_rva is None or col_off+20 > len(disk):
                continue
            sig, offset, cd, ptd, chd = struct.unpack_from('<IIIII',disk,col_off)
            if sig != 0 or ptd != td or raw(chd,16) is None:
                continue
            for col_ref in matches(disk, struct.pack('<I',base+col_rva)):
                vt_rva = rva(col_ref+4)
                if vt_rva is None or vt_rva % 4:
                    continue
                functions = raw(base+vt_rva,16)
                if functions is None or sum(code(fn) for fn in struct.unpack('<IIII',functions)) < 2:
                    continue
                entry = dict(className=class_name,typeDescriptorRva=hex(td_rva),
                             vtableRva=hex(vt_rva),completeObjectOffset=offset,
                             constructionOffset=cd)
                if entry not in found:
                    found.append(entry)
    return found


def stat_matches(data, values):
    """Coincidencias sao candidatos, nao campos confirmados por valor unico."""
    fields = {}
    for name, value in values.items():
        if value is None:
            continue
        hits = []
        for kind, fmt in [('double','<d'),('float','<f'),('uint32','<I'),('uint64','<Q')]:
            try:
                pattern = struct.pack(fmt, float(value) if kind in ('double','float') else int(value))
            except (ValueError, OverflowError, struct.error):
                continue
            for pos in matches(data,pattern):
                if pos % 4 == 0:
                    hits.append(dict(offset=hex(pos),type=kind))
        fields[name] = dict(status='value_candidates',value=value,candidates=hits)
    return fields


def position_matches(data, position):
    if position is None:
        return []
    x,y,z = position
    found = []
    # Layouts da familia OTC: inteiros 32 bits ou coordenadas compactas.
    for fmt, label, offsets in [('<III','u32_xyz',[0,4,8]),
                                ('<IIH','u32_u32_u16',[0,4,8]),
                                ('<HHB','u16_u16_u8',[0,2,4])]:
        for pos in matches(data,struct.pack(fmt,x,y,z)):
            found.append(dict(layout=label,offsets={key:hex(pos+off) for key,off in zip(('x','y','z'),offsets)}))
    return found


def name_matches(memory, data, name):
    if not name:
        return []
    encoded = name.encode('utf-8')
    found = [dict(offset=hex(pos),storage='inline_bytes') for pos in matches(data,encoded+b'\0')]
    for pos in range(0,len(data)-24+1,4):
        length, capacity = struct.unpack_from('<II',data,pos+16)
        if length != len(encoded) or not length <= capacity <= 4096:
            continue
        if capacity == 15 and length <= 15:
            content = data[pos:pos+length]
        elif capacity >= 16:
            pointer = struct.unpack_from('<I',data,pos)[0]
            if not 0x10000 <= pointer <= 0xFFFFFFFF-length:
                continue
            try:
                content = memory.exact(pointer,length)
            except (RuntimeError,OSError):
                continue
        else:
            continue
        if content == encoded:
            found.append(dict(offset=hex(pos),storage='msvc_x86_string_candidate'))
    return found


def inspect_object(memory, obj, vt, anchor, values, position, name, size=0x1000):
    data = memory.exact(obj,size)
    if struct.unpack_from('<I',data)[0] != vt:
        return None
    fields = stat_matches(data,values)
    names = name_matches(memory,data,name)
    positions = position_matches(data,position)
    hp_offset = anchor-obj if anchor is not None and obj <= anchor <= obj+size-8 else None
    field_hits = sum(bool(f['candidates']) for f in fields.values())
    return dict(objectAddress=hex(obj),vtableAddress=hex(vt),
                healthSeedOffset=hex(hp_offset) if hp_offset is not None else None,
                seedContained=hp_offset is not None,nameCandidates=names,
                positionCandidates=positions,fields=fields,
                score=10+20*bool(names)+20*bool(positions)+15*(hp_offset is not None)+field_hits,
                status='rtti_object_with_unconfirmed_fields')


def coverage():
    return dict(selected_bytes=0,read_bytes=0,failed_bytes=0,failed_pages=0,
                regions=0,failure_examples=[],complete=False,stopped_reason='running')


def discover(memory, report, values, position, name, fallback=False):
    path = memory.executable()
    base,size = memory.main_module(path)
    pe = parse_pe(lambda off,n: memory.exact(base+off,n) if 0 <= off <= size-n else b'')
    identity = validate_target(memory,path,base,size,pe)
    disk = Path(path).read_bytes()
    rtti = resolve_rtti(disk)
    primary = [r for r in rtti if not r['completeObjectOffset'] and not r['constructionOffset']]
    vts = {base+int(r['vtableRva'],16) for r in primary}
    if not vts:
        raise ValueError('RTTI LocalPlayer primario nao localizado nesta build; nenhuma varredura iniciada.')
    anchors = []
    report_note = 'no_seed'
    if report:
        import psutil
        session_time = psutil.Process(memory.pid).create_time()
        if (report.get('pid') != memory.pid or
            abs(report.get('process_create_time',0)-session_time) > 0.01 or
            report.get('targetValidation',{}).get('sha256') != DX['sha256'] or
            report.get('field') != 'hp'):
            raise ValueError('Relatorio HP nao pertence a este processo/build. Use --sem-seed para busca RTTI.')
        for candidate in report.get('candidates',[]):
            if candidate.get('type') != 'double':
                continue
            address = int(candidate['address'],0)
            if not 0x10000 <= address <= 0xFFFFFFF8:
                continue
            try:
                actual = struct.unpack('<d',memory.exact(address,8))[0]
            except (RuntimeError,OSError):
                continue
            if values.get('health') is not None and actual != values['health']:
                continue
            if math.isfinite(actual) and 0 < actual <= 1e7:
                anchors.append(address)
        report_note = 'seed_current_value_matches' if anchors else 'seed_unavailable_or_value_changed'
    objects = {}
    local_coverage = coverage()
    # A politica recuperada conserva o HP como ancora, sem deslocar a busca
    # para um atributo pequeno de outra alocacao.
    for anchor in anchors[:8]:
        start = max(0x10000, (anchor-0x2000) & ~3)
        length = anchor-start+8
        local_coverage['selected_bytes'] += length
        local_coverage['regions'] += 1
        for address,data in blocks(memory,start,length,local_coverage,4096):
            if data is None:
                continue
            for off in range((-address)%4,len(data)-3,4):
                vt = struct.unpack_from('<I',data,off)[0]
                if vt in vts:
                    obj = address+off
                    if obj <= anchor < obj+0x1000:
                        objects[obj] = (vt,anchor)
    local_coverage['complete'] = local_coverage['failed_bytes'] == 0
    local_coverage['stopped_reason'] = 'finished'
    scan_coverage = None
    if not objects and (fallback or not report):
        print('Buscando instancias LocalPlayer por vtable x86 em paginas gravaveis...',flush=True)
        scan_coverage = coverage()
        last_print = time.monotonic()
        for address,length in memory.regions('writable'):
            scan_coverage['regions'] += 1
            scan_coverage['selected_bytes'] += length
            for block,data in blocks(memory,address,length,scan_coverage):
                if data is None:
                    continue
                for vt in vts:
                    for off in matches(data,struct.pack('<I',vt)):
                        if (block+off)%4 == 0:
                            objects[block+off] = (vt,None)
                            if len(objects) > 4096:
                                raise ValueError('Instancias demais; busca interrompida sem promover perfil.')
            memory.alive()
            if time.monotonic()-last_print >= 3:
                print(f"[RTTI] {scan_coverage['read_bytes']/1048576:.1f} MiB lidos; {len(objects)} instancias",flush=True)
                last_print = time.monotonic()
        scan_coverage['complete'] = scan_coverage['failed_bytes'] == 0
        scan_coverage['stopped_reason'] = 'finished'
    candidates = []
    errors = []
    for obj,(vt,anchor) in objects.items():
        try:
            entry = inspect_object(memory,obj,vt,anchor,values,position,name)
            if entry:
                entry['vtableRva'] = hex(vt-base)
                candidates.append(entry)
        except (RuntimeError,OSError) as e:
            errors.append(dict(address=hex(obj),error=str(e)))
    candidates.sort(key=lambda c:c['score'],reverse=True)
    globals_found = []
    module_coverage = coverage()
    if candidates:
        wanted = {int(c['objectAddress'],0) for c in candidates}
        for s in pe['sections']:
            if s['name'] != '.data':
                continue
            length = s['virtual'] or s['raw']
            module_coverage['selected_bytes'] += length
            module_coverage['regions'] += 1
            for address,data in blocks(memory,base+s['rva'],length,module_coverage):
                if data is None:
                    continue
                for obj in wanted:
                    for off in matches(data,struct.pack('<I',obj)):
                        if (address+off)%4 == 0:
                            globals_found.append(dict(globalRva=hex(address+off-base),
                                                      objectAddress=hex(obj),status='single_snapshot_reference'))
    module_coverage['complete'] = module_coverage['failed_bytes'] == 0
    module_coverage['stopped_reason'] = 'finished' if candidates else 'not_needed'
    memory.alive()
    return dict(schema='cromin-ancientia-player-discovery-v1',pid=memory.pid,
                executable=path,moduleBase=hex(base),**identity,rtti=rtti,
                seedStatus=report_note,anchors=[hex(a) for a in anchors],
                observed=dict(name=name,position=position,**values),
                candidates=candidates,directGlobalCandidates=globals_found,
                coverage=dict(seedWindow=local_coverage,instanceScan=scan_coverage,moduleData=module_coverage),
                objectReadErrors=errors,
                status='object_candidates' if candidates else 'no_object_found',
                note='RTTI identifica a classe; atributos por coincidencia ainda exigem transicoes. Nao habilita acoes.')
