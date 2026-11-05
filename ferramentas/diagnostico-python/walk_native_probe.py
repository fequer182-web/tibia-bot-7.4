"""Confere candidatos nativos de walk na memoria. Nao executa funcoes internas."""
import argparse
from datetime import datetime
import json
from pathlib import Path
import struct
import sys

import psutil
from otc_monitor import Monitor, number
from value_scan import Memory, choose_pid


def inspect(monitor, manifest):
    if manifest.get('schema') != 'cromin-native-walk-static-v1':
        raise ValueError('Manifesto desconhecido')
    result = {'status':'read_only_preflight', 'native_call_executed':False,
              'base':hex(monitor.base),'functions':{},'static_bindings':{},
              'remaining':['execution_thread','return_type','third_argument_semantics','runtime_call']}
    for name,item in manifest['functions'].items():
        rva=number(item['rva'])
        expected=bytes.fromhex(item['first64'])
        current=monitor.module_read(rva,len(expected))
        if current != expected:
            raise ValueError(f'Bytes do candidato {name} diferem do manifesto. Teste interrompido.')
        result['functions'][name]={'rva':hex(rva),'address':hex(monitor.base+rva),
                                  'first64_match':True,'call_validated':False}
    for label,key,data_key in [('constructor','binding_constructor_rva','binding_constructor_first64'),
                               ('invoke','binding_invoke_rva','binding_invoke_first21')]:
        rva=number(manifest[key]);expected=bytes.fromhex(manifest[data_key])
        if monitor.module_read(rva,len(expected)) != expected:
            raise ValueError(f'Binding {label} diferente do manifesto')
        result['static_bindings'][label]={'rva':hex(rva),'bytes_match':True}
    rva=number(manifest['bound_instance_rva'])
    instance=monitor.base+rva
    data=monitor.module_read(rva,0x80)
    ptr0=struct.unpack_from('<Q',data,0)[0]
    ptr8=struct.unpack_from('<Q',data,8)[0]
    protocol=struct.unpack_from('<Q',data,0x30)[0]
    player=monitor.resolve_player()
    result['bound_instance']={'rva':hex(rva),'address':hex(instance),
        'qword_0':hex(ptr0),'qword_8':hex(ptr8),'qword_30':hex(protocol),
        'byte_60':data[0x60],'byte_61':data[0x61],'byte_62':data[0x62],
        'player_from_profile':hex(player),'qword_0_equals_player':ptr0==player,
        'qword_8_equals_player':ptr8==player}
    # Estas condicoes foram observadas no prologo; nao sao a validacao de todas as precondicoes.
    conditions={'instance_byte60_nonzero':data[0x60]!=0,'instance_byte62_zero':data[0x62]==0,
                'instance_pointer0_nonzero':ptr0!=0,'instance_pointer30_nonzero':protocol!=0}
    if ptr0:
        try:
            byte69=monitor.memory.exact(ptr0+0x69,1)[0]
            result['bound_instance']['pointer0_byte69']=byte69
            conditions['pointer0_byte69_nonzero']=byte69>0
        except RuntimeError as error:
            result['bound_instance']['pointer0_read_error']=str(error)
            conditions['pointer0_byte69_nonzero']=False
    result['observed_entry_checks']=conditions
    result['all_observed_entry_checks_pass']=all(conditions.values())
    result['position']=monitor.snapshot()['position']
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile',default='Antica_monitor.profile.json')
    parser.add_argument('--manifest',default=str(Path(__file__).with_name('native_walk_candidates.json')))
    parser.add_argument('--pid',type=int)
    parser.add_argument('--output',default='walk_native_probe_'+datetime.now().strftime('%Y%m%d_%H%M%S')+'.json')
    args=parser.parse_args()
    if sys.platform!='win32' or struct.calcsize('P')!=8:
        parser.error('Use Windows e Python 64 bits')
    memory=None
    try:
        output=Path(args.output)
        if output.exists():raise ValueError('Relatorio ja existe; escolha outro --output')
        profile=json.loads(Path(args.profile).read_text(encoding='utf-8-sig',errors='replace'))
        manifest=json.loads(Path(args.manifest).read_text(encoding='utf-8'))
        memory=Memory(choose_pid('Antica Retro opengl.exe',args.pid))
        monitor=Monitor(memory,profile)
        result=inspect(monitor,manifest)
        result['pid']=memory.pid
        result['executable']=monitor.path
        output.write_text(json.dumps(result,indent=2,ensure_ascii=False),encoding='utf-8')
        print(json.dumps(result,indent=2,ensure_ascii=False))
        print(f'[JSON] {output.resolve()}')
        print('Teste de leitura concluido. Nenhuma chamada interna executada.')
        return 0
    except (OSError,ValueError,RuntimeError,KeyError,TypeError,psutil.Error) as error:
        print(f'[ERRO] {error}',file=sys.stderr)
        return 1
    finally:
        if memory:memory.close()


if __name__=='__main__':raise SystemExit(main())
