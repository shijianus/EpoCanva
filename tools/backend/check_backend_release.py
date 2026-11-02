#!/usr/bin/env python3
"""Read-only backend gate. Writes only its report under build/<preset>."""
import argparse
import hashlib
import json
import pathlib
import subprocess
import xml.etree.ElementTree as ET

LANGUAGES=('en','zh','fr','ja','ko')

def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1<<20),b''):h.update(chunk)
    return h.hexdigest()

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--build-dir',type=pathlib.Path,required=True)
    parser.add_argument('--models',type=pathlib.Path,required=True)
    parser.add_argument('--junit',type=pathlib.Path,required=True)
    parser.add_argument('--output',type=pathlib.Path,required=True)
    args=parser.parse_args()
    root=pathlib.Path(__file__).resolve().parents[2]
    args.models=args.models.resolve()
    args.build_dir=args.build_dir.resolve()
    args.junit=args.junit.resolve()
    out=args.output.resolve()
    if not out.is_relative_to(root/'build'):raise ValueError('report must remain in build/')
    checks=[]
    def check(name, passed, detail):
        checks.append({'id':name,'status':'passed' if passed else 'blocked','detail':detail})
    source=subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip()
    dirty=subprocess.check_output(['git','-C',str(root),'status','--porcelain','--untracked-files=all'],text=True)
    check('clean-source',not dirty,'No uncommitted source may be relabeled as a stable artifact.')
    policy_path=root/'data/manifests/backend-readiness.json'
    policy=json.loads(policy_path.read_text(encoding='utf-8'))
    check('language-scope',policy['required_languages']==list(LANGUAGES),'Five ordered languages are mandatory; frontend scope is separate.')
    for language in LANGUAGES:
        rows=[r for r in policy['languages'] if r['id']==language]
        check('complete-'+language,len(rows)==1 and rows[0]['status']=='complete' and not rows[0]['blockers'],
              '; '.join(rows[0]['blockers']) if rows else 'missing language')
    try:
        xml_bytes=args.junit.read_bytes()
        if b'<!DOCTYPE' in xml_bytes or b'<!ENTITY' in xml_bytes:raise ValueError('DTD not allowed')
        xml=ET.fromstring(xml_bytes)
        cases=xml.findall('.//testcase')
        failed=xml.findall('.//failure')+xml.findall('.//error')+xml.findall('.//skipped')
        names=[c.get('name','') for c in cases]
        groups=('backend.headless.','backend_session.','prediction.core.','lexical.service.','japanese.decoder.','french.dictionary.')
        check('executed-backend-tests',bool(cases) and not failed and len(names)==len(set(names)) and all(any(n.startswith(g) for n in names) for g in groups),
              f'{len(cases)} actual JUnit cases; {len(failed)} failure/error/skip nodes.')
    except (OSError,ValueError,ET.ParseError) as error:
        check('executed-backend-tests',False,type(error).__name__)
    models=[]
    for language in LANGUAGES:
        model=args.models/language/'model.tsv'
        quality=args.models/language/'evaluation.json'
        report=args.models/language/'report.json'
        try:
            value=json.loads(quality.read_text(encoding='utf-8'))
            provenance=json.loads(report.read_text(encoding='utf-8'))
            actual=digest(model)
            bindings=value.get('inputs_sha256',{})
            measured=value.get('model',{})
            base=value.get('unigram_baseline',{})
            ci=value.get('bootstrap_95_percentile_ci',{}).get('delta.mrr',{})
            valid=value.get('quality_claim_eligible') is True and measured.get('mrr',0)>base.get('mrr',1) and ci.get('low_95',-1)>0
            binding_hash=bindings.get('model')
            check('quality-'+language,valid and binding_hash==actual,'Exact-model held-out MRR improvement with positive grouped confidence interval; semantic coverage is separately gated.')
            models.append({'language':language,'path':str(model.relative_to(root)) if model.is_absolute() else str(model),
                           'sha256':actual,'bytes':model.stat().st_size,'quality_sha256':digest(quality),'training_report_sha256':digest(report)})
        except (OSError,ValueError,KeyError) as error:
            check('quality-'+language,False,type(error).__name__)
    total=sum(row['bytes'] for row in models)
    check('aggregate-model-budget',len(models)==5 and total<=policy['limits']['aggregate_ngram_bytes'],f'{total} bytes across {len(models)} models; budget {policy["limits"]["aggregate_ngram_bytes"]}.')
    check('measured-rss-and-tail-latency',False,'Core query timing is not peak-RSS or complete synchronous backend p99 evidence.')
    check('source-reconstruction',False,'A source-only commit is not a verified offline reconstruction of third-party data/models and relinkable native libraries.')
    check('data-distribution',False,'Official notices recorded; complete model/word-list distribution package not yet verified.')
    check('neural-default-disabled',policy['limits']['neural_default_admitted'] is False,'Measured CPU reranker misses deadline; not silently enabled.')
    ready=all(c['status']=='passed' for c in checks)
    result={'format':1,'scope':policy['artifact_scope'],'source_commit':source,'policy_sha256':digest(policy_path),
            'stable_ready':ready,'checks':checks,'models':models,'exit_code':0 if ready else 1}
    out.parent.mkdir(parents=True,exist_ok=True)
    out.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print('backend_stable_ready='+str(ready).lower())
    for row in checks:
        if row['status']=='blocked':print('BLOCKED '+row['id']+': '+row['detail'])
    return result['exit_code']

if __name__=='__main__':raise SystemExit(main())
