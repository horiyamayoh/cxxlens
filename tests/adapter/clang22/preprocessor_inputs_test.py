"""Actual raw/evaluated preprocessor populations, distinct from returned matches."""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

analyzer, compiler = map(Path, sys.argv[1:3])
environment = dict(os.environ)
environment['CXXLENS_CLANG22_ANALYZER_JOBS'] = '1'
source = r'''
#if __has_include("unopened.hpp") && __has_include("malformed.hpp")
#endif
#define OBJECT 3
#define TWICE(value) ((value)+(value))
#define FORWARD(value) TWICE(value)
#define STRINGIFY(value) #value
#define UNUSED(value) 0
#define PASTE(a,b) a##b
#define VARIADIC(first,...) first __VA_OPT__(+ __VA_ARGS__)
#if FEATURE
#define SELECTED 1
#if 0
#define INACTIVE(x) ((x)+(x))
#else
#define NESTED 2
#endif
#else
#define SELECTED 0
#endif
int sample() {
 int value=1;
 int twice=TWICE(value++);
 auto text=STRINGIFY(value++);
 int unused=UNUSED(value++);
 auto unevaluated=sizeof(TWICE(value++));
 int forwarded=FORWARD(value++);
 int pasted=PASTE(1,2);
 return forwarded+twice+unused+int(unevaluated)+pasted+OBJECT+VARIADIC(1,2)+SELECTED+NESTED;
}
'''

def values(row):
    return {key.removeprefix('output.'): value.get('value') for key, value in row['values'].items()}

def elements(encoded):
    raw = bytes.fromhex(encoded)
    result = []
    while raw:
        length = int.from_bytes(raw[:4], 'little')
        result.append(raw[4:4+length].decode())
        raw = raw[4+length:]
    assert result == sorted(set(result))
    return result

with tempfile.TemporaryDirectory(prefix='cxxlens-pp-inputs-') as directory:
    root=Path(directory)
    (root/'main.cpp').write_text(source)
    (root/'unopened.hpp').write_text('#define UNOPENED(x) #x\n')
    (root/'malformed.hpp').write_text('#define DUPLICATE(x,x) x\n#define LEADING ## x\n#define TRAILING x ##\n')
    (root/'compile_commands.json').write_text(json.dumps([{
        'directory': str(root), 'file': 'main.cpp',
        'arguments': [str(compiler), '-std=c++23', '-DFEATURE=1', '-c', 'main.cpp', '-o', 'main.o']
    }]))
    run=subprocess.run([str(analyzer), '--project-root',str(root),'--compile-commands',str(root/'compile_commands.json')],
                       text=True,capture_output=True,env=environment,timeout=240)
    assert run.returncode == 0, run.stderr
    bundle=json.loads(run.stdout)
    scans={q['logical_ir']['relation_requirements'][0]['descriptor_id']:q['result'] for q in bundle['queries']}
    rows={name:[values(r) for r in scan['rows']] for name,scan in scans.items()}
    events={r['event']:r for r in rows['source.preprocessor_event.v1']}
    spans={r['span']:r for r in rows['source.span.v1']}
    files={r['file']:r for r in rows['source.file.v1']}
    assert {r['logical_path'] for r in files.values()} == {
        'project://root/main.cpp', 'project://root/unopened.hpp', 'project://root/malformed.hpp'
    }
    syntax={r['node']:r for r in rows['cc.syntax_node.v1']}
    inventories=rows['source.preprocessor_inventory.v1']
    assert inventories, run.stderr
    for inventory in inventories:
        population=[events[i] for i in elements(inventory['event_ids'])]
        assert inventory['event_count'] == len(population), inventory
        assert all(r['phase']==inventory['phase'] for r in population)
        assert all(spans[r['source']]['file']==inventory['file'] for r in population)
        if inventory['phase']=='raw':
            assert inventory['enumeration_state']=='complete'
            assert inventory['structure_state']=='complete'
            path=files[inventory['file']]['logical_path']
            assert inventory['macro_state']==('partial' if path=='project://root/malformed.hpp' else 'complete'), inventory
            primary=[r for r in population if r['raw_event'] is None]
            raw_tokens=[r for r in rows['source.token.v1'] if r['phase']=='raw' and spans[r['source']]['file']==inventory['file']]
            admitted={r['token'] for r in raw_tokens if r['directive_start']}
            returned={token for r in primary for token in elements(r['raw_token_ids']) if token in admitted}
            assert returned == admitted
        if files[inventory['file']]['logical_path'] in {'project://root/unopened.hpp','project://root/malformed.hpp'} and inventory['phase']=='evaluated':
            assert inventory['enumeration_state']=='unavailable' and inventory['event_count']==0, inventory
    malformed=[r for r in events.values() if r['phase']=='raw' and files[spans[r['source']]['file']]['logical_path']=='project://root/malformed.hpp']
    assert len([r for r in malformed if r['kind']=='raw_define'])==3
    assert all(r['macro_state']=='partial' for r in malformed), malformed
    raw_main=[r for r in events.values() if r['phase']=='raw' and files[spans[r['source']]['file']]['logical_path']=='project://root/main.cpp']
    openings=[r for r in raw_main if r['kind']=='raw_if']
    assert {r['depth'] for r in openings} == {1,2}
    for opening in openings:
        assert events[opening['closing_event']]['kind']=='raw_endif'
        assert opening['region_begin'] <= opening['region_end']
    inactive=next(r for r in raw_main if r['name']=='INACTIVE' and r['kind']=='raw_define')
    assert inactive['activity']=='inactive', inactive
    for definition in [r for r in raw_main if r['kind']=='raw_define']:
        parameters=[r for r in raw_main if r['kind']=='macro_parameter' and r['raw_event']==definition['event']]
        assert {r['parameter_index'] for r in parameters} == set(range(definition['parameter_count']))
        assert all(r['macro_state']=='complete' for r in parameters)
    operators=[r for r in raw_main if r['kind']=='macro_replacement_operator']
    assert operators and all(r['macro_state']=='complete' and r['paste_token_ids'] is not None and r['stringify_token_ids'] is not None for r in operators)
    arguments=[r for r in events.values() if r['kind']=='macro_argument']
    twice=[r for r in arguments if events[r['parent_event']]['name']=='TWICE']
    assert sorted(r['evaluating_substitution_count'] for r in twice)==[0,2,2], twice
    for argument in twice:
        assert argument['substitution_count']==2 and argument['argument_may_have_side_effects'] is True
        assert argument['effect_state']=='complete' and argument['effect_expression'] in syntax
        assert syntax[argument['effect_expression']]['function']==argument['effect_function']
    forwarded=[r for r in arguments if events[r['parent_event']]['name']=='FORWARD']
    assert len(forwarded)==1 and forwarded[0]['substitution_count']==1 and forwarded[0]['evaluating_substitution_count']==2, forwarded
    no_evaluation=[r for r in arguments if events[r['parent_event']]['name'] in {'STRINGIFY','UNUSED'}]
    assert len(no_evaluation)==2 and all(r['evaluating_substitution_count']==0 for r in no_evaluation)
    # Unused/stringified operands have no invented AST expression or semantic side-effect truth.
    assert all(r['effect_expression'] is None and r['effect_state']=='partial' for r in no_evaluation)
print('actual raw/evaluated preprocessor oracle passed')
