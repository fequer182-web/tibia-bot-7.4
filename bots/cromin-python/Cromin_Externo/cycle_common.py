import json
import os
from pathlib import Path
import subprocess

class SkipCycle(ValueError):
    """World evidence unavailable: skip this action, keep the session connected."""


def run_child(command, prefix):
    env = os.environ.copy()
    env['PYTHONIOENCODING']='utf-8'
    child = subprocess.Popen([str(v) for v in command],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                             text=True,encoding='utf-8',errors='replace',env=env,
                             creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    lines=[]
    # O controlador nao mata uma chamada em andamento por timeout ou Ctrl+C.
    for line in child.stdout:
        print(line,end='',flush=True);lines.append(line.rstrip())
    code=child.wait()
    path=next((Path(v.removeprefix(prefix)) for v in reversed(lines) if v.startswith(prefix)),None)
    data=json.loads(path.read_text(encoding='utf-8')) if path and path.is_file() else None
    return {'exit_code':code,'output':lines,'report_path':str(path) if path else None,'report':data}


def native_completed(result, extras=()):
    data=result['report']
    return bool(result['exit_code']==0 and data and all(data.get(k) for k in (
        'started','returned','context_restored','stack_slot_restored','detached',*extras)) and not data.get('diagnostics'))
