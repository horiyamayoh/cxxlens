#!/usr/bin/env python3
"""Properties and negative tests for the NG semantic guarantee algebra."""
from __future__ import annotations
import itertools, pathlib, sys, unittest
ROOT=pathlib.Path(__file__).resolve().parents[2]; sys.path.insert(0,str(ROOT/'tools/quality'))
from check_ng_semantic_guarantee import APP, CONTRACT, SemanticError, approximation, compare_confidence, compare_modalities, compose_metadata, digest, filter_truth, load, summarize, truth, validate_all, validate_compression, validate_exact

class SemanticGuaranteeTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls): cls.contract=load(ROOT/CONTRACT)
 def test_contract_and_vectors(self):
  contract,results,comparisons=validate_all(ROOT); self.assertEqual(len(contract['operator_composition']),12); self.assertEqual(len(results),24); self.assertEqual(comparisons,6)
 def test_truth_not_involution(self):
  for value in ('unknown','true','false','conflict'): self.assertEqual(truth('not',[truth('not',[value])]),value)
 def test_truth_and_or_are_commutative(self):
  for a,b in itertools.product(('unknown','true','false','conflict'),repeat=2):
   self.assertEqual(truth('and',[a,b]),truth('and',[b,a])); self.assertEqual(truth('or',[a,b]),truth('or',[b,a]))
 def test_unknown_and_conflict_are_not_bool(self):
  self.assertFalse(filter_truth('unknown','true_only')); self.assertFalse(filter_truth('conflict','true_only'))
 def test_approximation_meet_is_order_invariant(self):
  for a,b in itertools.product(APP,repeat=2): self.assertEqual(approximation([a,b]),approximation([b,a]))
 def test_verification_is_partial_order(self):
  self.assertEqual(compare_modalities(self.contract,['compiler_verified'],['runtime_observed']),'incomparable'); self.assertEqual(compare_modalities(self.contract,['frontend_replayed'],['schema_validated']),'stronger')
 def test_exact_rejects_coverage_and_closure_gaps(self):
  base={'approximation':'exact','scope':'s','interpretation':'i','coverage':['covered'],'closures':['c'],'requires_closure':True,'condition_partition_complete':True,'assumptions':[],'conflict':False,'unresolved':False}; validate_exact(base)
  broken=dict(base,coverage=['unresolved'])
  with self.assertRaisesRegex(SemanticError,'exact-coverage'): validate_exact(broken)
  broken=dict(base,closures=[])
  with self.assertRaisesRegex(SemanticError,'exact-closure'): validate_exact(broken)
 def test_join_retains_all_contributors(self):
  values=[{'approximation':'exact','scope':'a','condition':{'universe':'u','alternatives':['x']},'interpretation':'i','assumptions':[],'verification_modalities':['compiler_verified'],'contributors':['c1'],'provenance':['p1']},{'approximation':'exact','scope':'b','condition':{'universe':'u','alternatives':['x']},'interpretation':'i','assumptions':[],'verification_modalities':['runtime_observed'],'contributors':['c2'],'provenance':['p2']}]
  result=compose_metadata(self.contract,values,'join'); self.assertEqual(result['contributors'],['c1','c2']); self.assertEqual(result['provenance'],['p1','p2']); self.assertEqual(result['verification_modalities'],['schema_validated'])
 def test_summary_never_upgrades_under_fragment(self):
  fragments=[{'approximation':'exact','coverage':['covered'],'condition_partition_complete':True,'conflict':False,'unresolved':False,'requires_closure':False,'closures':[],'assumptions':[],'verification_modalities':[]},{'approximation':'under_approximation','coverage':['covered'],'condition_partition_complete':True,'conflict':False,'unresolved':False,'requires_closure':False,'closures':[],'assumptions':[],'verification_modalities':[]}]
  result=summarize(self.contract,fragments); self.assertEqual(result['approximation'],'under_approximation'); self.assertEqual(result['fragment_count'],2); self.assertTrue(result['drill_down_ref'])
 def test_compression_requires_resolvable_exact_child_set(self):
  children=['p1','p2']; validate_compression({'algorithm_id':'a','canonical_child_root_ids':children,'child_count':2,'child_set_digest':digest(children),'resolver_ref':'cas','retention_policy':'compressed'})
  with self.assertRaisesRegex(SemanticError,'compression-lossy'): validate_compression({'algorithm_id':'a','canonical_child_root_ids':children,'child_count':2,'child_set_digest':digest(children),'resolver_ref':'','retention_policy':'compressed'})
 def test_confidence_requires_same_calibration_domain(self):
  a={'value':.9,'calibration_id':'c1','population_id':'p','metric':'precision'}; b=dict(a,value=.8); self.assertEqual(compare_confidence(a,b),'higher'); self.assertEqual(compare_confidence(a,dict(b,calibration_id='c2')),'incomparable')

if __name__=='__main__': unittest.main()
