from pathlib import Path
import subprocess,sys
root=Path(__file__).resolve().parents[1]
commands=[['tests/test_nested_food.py'],['tests/test_item_loot.py'],['tests/test_follow_recovery.py'],['tests/test_look.py'],['tests/test_map_snapshot.py'],['tests/test_follow.py'],['tests/test_targeting.py'],['tests/test_profiles.py'],['tests/test_controller.py'],['tests/test_target_loss.py'],['tests/test_skip.py'],['tests/test_light.py'],['iniciar.py','--self-test']]
for command in commands:
    print('\nTESTE:',command[0],flush=True)
    result=subprocess.run([sys.executable,str(root/command[0]),*command[1:]],cwd=root)
    if result.returncode:raise SystemExit(result.returncode)
print('\nTodos os testes locais passaram. Nenhuma conexao ao Antica foi solicitada.')
