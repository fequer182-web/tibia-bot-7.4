"""Prepare an isolated source tree and compile only; never starts/stops a server."""
import hashlib,json,os,shutil,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parent

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def prepare(source,lab):
    source=source.resolve();lab=lab.resolve()
    if lab.is_relative_to(source) or source.is_relative_to(lab):raise ValueError('Laboratorio e servidor precisam ficar em pastas separadas.')
    manifest=json.loads((ROOT/'manifest.json').read_text(encoding='utf-8-sig'))
    for name,entry in manifest.items():
        if not (source/name).exists() or sha(source/name)!=entry['originalSha256']:
            raise ValueError('Seu codigo de origem mudou ou arquivo ausente: '+name+'. Nao aplicar esta versao automaticamente.')
    identity=dict(source=str(source),patchSha256=sha(ROOT/'manifest.json'))
    marker=lab/'crome_lab.json'
    if lab.exists():
        if not marker.exists() or json.loads(marker.read_text())!=identity:
            raise ValueError('Pasta de laboratorio ja existe com outros arquivos/versao. Configure outra pasta; nada sera apagado.')
        print('Laboratorio existente preservado; apenas recompilar.')
    else:
        lab.mkdir(parents=True)
        for directory in ['src','vc17','cmake']:shutil.copytree(source/directory,lab/'source'/directory)
        for name in manifest:
            destination=lab/'source'/name;shutil.copy2(ROOT/name,destination)
            if sha(destination)!=manifest[name]['patchedSha256']:raise ValueError('Falha ao verificar copia: '+name)
        destination=lab/'run_files/data/scripts';destination.mkdir(parents=True)
        shutil.copy2(ROOT/'data/scripts/crome_internal_bot.lua',destination/'crome_internal_bot.lua')
        marker.write_text(json.dumps(identity,indent=2),encoding='utf-8')
    return lab/'source/vc17/theforgottenserver.vcxproj'

def compile_project(project,lab):
    vswhere=Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'))/'Microsoft Visual Studio/Installer/vswhere.exe'
    if not vswhere.exists():raise RuntimeError('Visual Studio Installer nao encontrado. Instale o workload Desenvolvimento para desktop com C++.')
    result=subprocess.run([str(vswhere),'-products','*','-format','json'],capture_output=True,text=True,encoding='utf-8')
    candidates=json.loads(result.stdout or '[]')
    for instance in candidates:
        base=Path(instance['installationPath'])
        if (base/'MSBuild/Microsoft/VC/v170/Microsoft.Cpp.Default.props').exists() and (base/'VC/Tools/MSVC').exists():break
    else:raise RuntimeError('Componente C++ ausente. No Visual Studio Installer, Modificar Build Tools2022: adicione Desenvolvimento para desktop com C++, MSVCv143 e WindowsSDK. Dependencias C++ do TFS tambem precisam estar configuradas.')
    msbuild=base/'MSBuild/Current/Bin/MSBuild.exe'
    build=lab/'build';build.mkdir(exist_ok=True)
    log=build/'compilacao.log'
    args=[str(msbuild),str(project),'/p:Configuration=Release','/p:Platform=x64','/p:TargetName=crome-internal-bot','/p:OutDir='+str(build/'bin')+'\\','/p:IntDir='+str(build/'obj')+'\\','/v:minimal','/nologo']
    print('Compilando copia isolada. Log:',log,flush=True)
    with log.open('w',encoding='utf-8') as output:result=subprocess.run(args,cwd=project.parent,stdout=output,stderr=subprocess.STDOUT)
    if result.returncode:
        print(log.read_text(encoding='utf-8',errors='replace')[-7000:]);raise RuntimeError('Compilacao nao concluida; conferir dependencias e log.')
    print('Compilacao concluida:',build/'bin/crome-internal-bot.exe')
    print('Nenhum servidor iniciado. Leia LEIA-ME antes do teste em uma instancia separada.')

def main():
    cfg=json.loads((ROOT/'configuracao.json').read_text(encoding='utf-8-sig'))
    source=Path(cfg['servidor_origem']);lab=Path(cfg['laboratorio'])
    project=prepare(source,lab)
    print('Codigo preparado:',project)
    compile_project(project,lab)
if __name__=='__main__':
    try:main()
    except Exception as error:print('BLOQUEIO:',error);sys.exit(1)
