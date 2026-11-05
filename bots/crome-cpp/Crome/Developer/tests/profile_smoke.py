import json, pathlib, sys
base=pathlib.Path(__file__).resolve().parents[1]/'profiles'
files=list(base.glob('*.json'))
assert files, 'nenhum profile'
for p in files:
    j=json.loads(p.read_text(encoding='utf-8'))
    assert j['schema']=='otclab-build-profile-v2'
    assert j['schemaVersion']==2
    assert 'build' in j and 'values' in j and 'migratorState' in j and 'features' in j
    assert j['build']['architecture'] in ('x64','x86')
    print(p.name, j.get('profileStatus'), j['build']['fingerprint']['textHashFNV1A64'])
print('OK:',len(files),'profiles')
