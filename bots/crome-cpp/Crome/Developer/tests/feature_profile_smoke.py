import json, pathlib
base=pathlib.Path(__file__).resolve().parents[1]/'profiles'
for p in sorted(base.glob('OTCLab_BuildProfile_*.json')):
    j=json.loads(p.read_text(encoding='utf-8'))
    f=j.get('features',{})
    assert 'look' in f and 'hackLight' in f, f'{p.name}: missing features contract'
    look=f['look']; light=f['hackLight']
    valid={'confirmed_runtime','confirmed_runtime_write','candidate','pending'}
    assert look.get('status') in valid
    assert light.get('status') in valid
    if look.get('status','').startswith('confirmed'):
        assert int(str(look['hookRva']),0) > 0
        assert int(str(look['itemIdOffset']),0) > 0
    if light.get('status','').startswith('confirmed'):
        assert light.get('storage') == 'local_player_field'
        assert light.get('type') == 'u8'
        assert int(str(light['intensityOffset']),0) > 0
        assert light.get('validation',{}).get('restoreConfirmed') is True
    print(p.name, 'look=',look.get('status'),'hackLight=',light.get('status'))
print('OK: feature profile contract')
