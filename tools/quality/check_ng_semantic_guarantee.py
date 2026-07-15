#!/usr/bin/env python3
"""Executable truth, guarantee, condition, and provenance algebra for Issue #62."""
from __future__ import annotations
import argparse, copy, hashlib, json, pathlib, random, sqlite3, sys
from typing import Any
import jsonschema, yaml

ROOT=pathlib.Path(__file__).resolve().parents[2]
CONTRACT=pathlib.Path('schemas/cxxlens_ng_semantic_guarantee_contract.yaml')
CONTRACT_SCHEMA=pathlib.Path('schemas/cxxlens_ng_semantic_guarantee_contract.schema.yaml')
METADATA_SCHEMA=pathlib.Path('schemas/cxxlens_ng_semantic_metadata.schema.yaml')
VECTORS=pathlib.Path('schemas/cxxlens_ng_semantic_conformance_vectors.yaml')
VECTORS_SCHEMA=pathlib.Path('schemas/cxxlens_ng_semantic_conformance_vectors.schema.yaml')
REPORT_SCHEMA=pathlib.Path('schemas/cxxlens_ng_semantic_conformance_report.schema.yaml')

class SemanticError(ValueError):
    def __init__(self,code:str,message:str): super().__init__(f'{code}: {message}'); self.code=code
def fail(code:str,message:str)->None: raise SemanticError(code,message)
def load(path:pathlib.Path)->dict[str,Any]:
    value=yaml.safe_load(path.read_text());
    if not isinstance(value,dict): fail('semantic.document-invalid',str(path))
    return value
def validate_schema(value:Any,schema:dict[str,Any],label:str)->None:
    try: jsonschema.Draft202012Validator.check_schema(schema); jsonschema.Draft202012Validator(schema).validate(value)
    except (jsonschema.SchemaError,jsonschema.ValidationError) as error: fail('semantic.schema-invalid',f'{label}: {error.message}')
def canonical(value:Any)->bytes: return json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=False).encode()
def digest(value:Any)->str: return 'sha256:'+hashlib.sha256(canonical(value)).hexdigest()

TRUTH={'unknown':(False,False),'true':(True,False),'false':(False,True),'conflict':(True,True)}
def truth_name(value:tuple[bool,bool])->str: return next(k for k,v in TRUTH.items() if v==value)
def truth(op:str,values:list[str])->str:
    pairs=[TRUTH[v] for v in values]
    if op=='not': return truth_name((pairs[0][1],pairs[0][0]))
    if op=='and': return truth_name((all(x[0] for x in pairs),any(x[1] for x in pairs)))
    if op=='or': return truth_name((any(x[0] for x in pairs),all(x[1] for x in pairs)))
    if op=='evidence': return truth_name((any(x[0] for x in pairs),any(x[1] for x in pairs)))
    fail('semantic.truth-operator-unknown',op)
def filter_truth(value:str,policy:str)->bool:
    if policy=='true_only': return value=='true'
    if policy=='retain_unknown': return value in {'true','unknown'}
    if policy=='retain_conflict': return value in {'true','conflict'}
    if policy=='retain_all': return True
    if policy=='strict_known': return value in {'true','false'}
    if policy=='strict_nonconflicting': return value!='conflict'
    fail('semantic.filter-policy-unknown',policy)

APP={'unknown':(False,False),'under_approximation':(True,False),'over_approximation':(False,True),'exact':(True,True)}
def approximation(values:list[str],operator:str='meet')->str:
    bits=[APP[v] for v in values]
    result=(all(x[0] for x in bits),all(x[1] for x in bits))
    if operator=='limit' and result==(True,True): result=(True,False)
    return next(k for k,v in APP.items() if v==result)
def modality_closure(contract:dict[str,Any],values:list[str])->set[str]:
    rows={r['id']:r for r in contract['verification']['modalities']}; result=set(values); pending=list(values)
    while pending:
        current=pending.pop()
        if current not in rows: continue
        for implied in rows[current]['implies']:
            if implied not in result: result.add(implied); pending.append(implied)
    return result
def compare_modalities(contract:dict[str,Any],left:list[str],right:list[str])->str:
    a=modality_closure(contract,left); b=modality_closure(contract,right)
    if a==b:return 'equal'
    if a>b:return 'stronger'
    if a<b:return 'weaker'
    return 'incomparable'
def compose_modalities(contract:dict[str,Any],sets:list[list[str]])->list[str]:
    closures=[modality_closure(contract,x) for x in sets]
    return sorted(set.intersection(*closures)) if closures else []

BLOCK={'failed','unresolved','unsupported','stale','truncated'}
def validate_exact(value:dict[str,Any])->None:
    if value['approximation']!='exact': return
    if not value.get('scope') or not value.get('interpretation'): fail('semantic.exact-scope-missing','scope/interpretation')
    if BLOCK & set(value.get('coverage',[])): fail('semantic.exact-coverage-incomplete','blocking coverage')
    if value.get('requires_closure') and not value.get('closures'): fail('semantic.exact-closure-missing','closure')
    if value.get('conflict') or value.get('unresolved'): fail('semantic.exact-overlap-invalid','conflict/unresolved')
    if not value.get('condition_partition_complete',False): fail('semantic.exact-condition-incomplete','condition')
    if 'assumptions' not in value: fail('semantic.exact-assumptions-missing','assumptions')
def compose_metadata(contract:dict[str,Any],values:list[dict[str,Any]],operator:str)->dict[str,Any]:
    if not values: fail('semantic.compose-empty','inputs')
    universes={v['condition']['universe'] for v in values}; interpretations={v['interpretation'] for v in values}
    if len(universes)!=1: fail('semantic.condition-universe-mismatch','universe')
    if operator in {'join','derivation'} and len(interpretations)!=1: fail('semantic.interpretation-mismatch','domain')
    conditions=[set(v['condition']['alternatives']) for v in values]
    alternatives=set.intersection(*conditions) if operator=='join' else set.union(*conditions)
    if not alternatives: fail('semantic.condition-empty','composition')
    return {'approximation':approximation([v['approximation'] for v in values]),'scope':sorted(v['scope'] for v in values),'condition':{'universe':next(iter(universes)),'alternatives':sorted(alternatives)},'interpretations':sorted(interpretations),'assumptions':sorted({x for v in values for x in v['assumptions']}),'verification_modalities':compose_modalities(contract,[v['verification_modalities'] for v in values]),'contributors':sorted({x for v in values for x in v['contributors']}),'provenance':sorted({x for v in values for x in v['provenance']})}
def summarize(contract:dict[str,Any],fragments:list[dict[str,Any]])->dict[str,Any]:
    if not fragments: fail('semantic.summary-empty','fragments')
    exact=all(f['approximation']=='exact' and not (BLOCK&set(f['coverage'])) and f.get('condition_partition_complete') and not f.get('conflict') and not f.get('unresolved') and (not f.get('requires_closure') or f.get('closures')) for f in fragments)
    result={'approximation':'exact' if exact else approximation([f['approximation'] for f in fragments]),'assumptions':sorted({x for f in fragments for x in f['assumptions']}),'verification_modalities':compose_modalities(contract,[f['verification_modalities'] for f in fragments]),'fragment_count':len(fragments),'fragment_set_digest':digest(sorted(digest(f) for f in fragments)),'drill_down_ref':'fragments:'+digest(sorted(digest(f) for f in fragments))[7:]}
    if result['approximation']=='exact' and not exact: fail('semantic.summary-exact-overclaim','summary')
    return result
def validate_compression(value:dict[str,Any])->None:
    required={'algorithm_id','canonical_child_root_ids','child_count','child_set_digest','resolver_ref','retention_policy'}
    if set(value)!=required: fail('semantic.provenance-compression-lossy','fields')
    if value['child_count']!=len(value['canonical_child_root_ids']) or not value['resolver_ref']: fail('semantic.provenance-compression-lossy','children')
    if value['child_set_digest']!=digest(sorted(value['canonical_child_root_ids'])): fail('semantic.provenance-compression-digest','digest')
def compare_confidence(a:dict[str,Any],b:dict[str,Any])->str:
    keys=('calibration_id','population_id','metric')
    if any(a[k]!=b[k] for k in keys): return 'incomparable'
    return 'equal' if a['value']==b['value'] else ('higher' if a['value']>b['value'] else 'lower')

def execute(contract:dict[str,Any],vector:dict[str,Any])->dict[str,Any]:
    value=vector['input']; op=vector['operation']
    try:
        if op=='truth': output=truth(value['operator'],value['values'])
        elif op=='filter':
            before=value['truth']; output={'selected':filter_truth(before,value['policy']),'truth':before}
            if value.get('coerce_to_bool') and before in {'unknown','conflict'}: fail('semantic.truth-coercion-forbidden',before)
        elif op=='compare_verification': output=compare_modalities(contract,value['left'],value['right'])
        elif op=='compose_approximation': output=approximation(value['values'],value.get('operator','meet'))
        elif op=='validate_exact': validate_exact(value); output='valid'
        elif op=='compose_metadata': output=compose_metadata(contract,value['values'],value['operator'])
        elif op=='summarize': output=summarize(contract,value['fragments'])
        elif op=='validate_compression': validate_compression(value); output='valid'
        elif op=='compare_confidence': output=compare_confidence(value['left'],value['right'])
        elif op=='backend_matrix':
            outputs=[]
            for backend in ('memory','sqlite'):
                for order in ('forward','reverse','seeded-shuffle'):
                    rows=copy.deepcopy(value['values'])
                    if order=='reverse': rows.reverse()
                    elif order=='seeded-shuffle': random.Random(62).shuffle(rows)
                    if backend=='sqlite':
                        db=sqlite3.connect(':memory:'); db.execute('create table x(v text)'); db.executemany('insert into x values(?)',[(canonical(x).decode(),) for x in rows]); rows=[json.loads(x[0]) for x in db.execute('select v from x')]
                    outputs.append(compose_metadata(contract,rows,value['operator']))
            if len({canonical(x) for x in outputs})!=1: fail('semantic.backend-mismatch','matrix')
            output=outputs[0]
        else: fail('semantic.operation-unknown',op)
        return {'decision':'accepted','reason_code':f'semantic.{op}-valid','value':output}
    except SemanticError as error: return {'decision':'rejected','reason_code':error.code}

def validate_all(root:pathlib.Path)->tuple[dict[str,Any],list[dict[str,Any]],int]:
    contract=load(root/CONTRACT); validate_schema(contract,load(root/CONTRACT_SCHEMA),'contract')
    try: jsonschema.Draft202012Validator.check_schema(load(root/METADATA_SCHEMA))
    except jsonschema.SchemaError as error: fail('semantic.schema-invalid',f'metadata: {error.message}')
    if contract['verification']['ordinal_comparison']!='forbidden': fail('semantic.verification-ordinal-forbidden','contract')
    if contract['truth']['coercions']['unknown_to_false']!='forbidden' or contract['truth']['filtering']['mutates_truth']: fail('semantic.truth-coercion-forbidden','contract')
    expected={'query.scan.v1','query.filter.v1','query.project.v1','query.inner_join.v1','query.semi_join.v1','query.union.v1','query.distinct.v1','query.order_by.v1','query.limit.v1','query.condition_restrict.v1','query.interpretation_restrict.v1','derivation'}
    if {row['operator'] for row in contract['operator_composition']}!=expected: fail('semantic.operator-table-incomplete','operators')
    design=(root/'docs/design/cxxlens_next_generation_integrated_design_ja.md').read_text()
    for marker in ('1.0.0-normative','cxxlens_ng_semantic_guarantee_contract.yaml','knowledge order','modality set','summary_guarantee()','Issue #62'):
        if marker not in design: fail('semantic.design-marker-missing',marker)
    index=(root/'docs/design/catalogs/README.md').read_text()
    if 'Semantic Guarantee Contract' not in index or '#62' not in index: fail('semantic.catalog-index-stale','index')
    vectors=load(root/VECTORS); validate_schema(vectors,load(root/VECTORS_SCHEMA),'vectors')
    if len(contract['operator_composition'])!=12: fail('semantic.operator-table-incomplete','operators')
    results=[]; comparisons=0
    for vector in vectors['vectors']:
        actual=execute(contract,vector); expected=vector['expected']
        if actual['decision']!=expected['decision'] or actual['reason_code']!=expected['reason_code'] or ('value'in expected and actual.get('value')!=expected['value']): fail('semantic.vector-mismatch',vector['id'])
        if (vector['class']=='positive')!=(actual['decision']=='accepted'): fail('semantic.vector-class-mismatch',vector['id'])
        results.append({'id':vector['id'],**actual,'matched':True}); comparisons+=6 if vector['operation']=='backend_matrix' else 0
    report={'schema':'cxxlens.semantic-conformance-report.v1','contract_digest':digest(contract),'vector_results':results,'backend_matrix':{'backends':['memory','sqlite'],'orders':['forward','reverse','seeded-shuffle'],'comparisons':comparisons,'all_equal':True},'status':'green'}
    validate_schema(report,load(root/REPORT_SCHEMA),'report'); return contract,results,comparisons
def main()->int:
    p=argparse.ArgumentParser(); p.add_argument('mode',choices=('check','report')); p.add_argument('--root',type=pathlib.Path,default=ROOT); p.add_argument('--output',type=pathlib.Path); a=p.parse_args(); c,r,n=validate_all(a.root.resolve()); report={'schema':'cxxlens.semantic-conformance-report.v1','contract_digest':digest(c),'vector_results':r,'backend_matrix':{'backends':['memory','sqlite'],'orders':['forward','reverse','seeded-shuffle'],'comparisons':n,'all_equal':True},'status':'green'}
    if a.mode=='report': (a.output.write_text(json.dumps(report,indent=2,sort_keys=True)+'\n') if a.output else print(json.dumps(report,indent=2,sort_keys=True)))
    print(f'verified semantic algebra: {len(r)} vectors, {n} backend comparisons, {digest(c)}'); return 0
if __name__=='__main__':
    try: raise SystemExit(main())
    except (OSError,SemanticError) as error: print(f'semantic guarantee failure: {error}',file=sys.stderr); raise SystemExit(1)
