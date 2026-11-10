#!/usr/bin/env python3
"""Replay an authored case in fresh processes; keep all diagnostics in build/.

This is a regression protocol, not corpus quality qualification. The ordered
candidate signature includes semantic identity, source, score and UTF-8 bytes;
transient menu tokens and measured latency are deliberately excluded.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]


def confined(path: Path) -> Path:
    if '..' in path.parts:
        raise ValueError('artifact traversal')
    resolved = path.resolve()
    if resolved == (REPO / 'build').resolve() or not resolved.is_relative_to((REPO / 'build').resolve()):
        raise ValueError('artifact must be strictly inside canonical repo/build')
    return resolved


def signature(candidates: list[dict]) -> bytes:
    fields = ('rank', 'kind', 'text_utf8', 'score', 'lexeme_ids', 'stable_id',
              'language_tag', 'scheme_id', 'source', 'match', 'commit', 'learnable',
              'input_begin_utf8_byte', 'input_end_utf8_byte')
    return json.dumps([{key: row[key] for key in fields} for row in candidates],
                      ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode('utf-8')


def assess(processes: list[list[bytes]], repeat: int) -> dict:
    if not processes or repeat < 2 or any(len(rows) != repeat for rows in processes):
        raise ValueError('incomplete stress matrix')
    reference = processes[0][0]
    hashes = [[hashlib.sha256(value).hexdigest() for value in rows] for rows in processes]
    within = all(all(value == rows[0] for value in rows) for rows in processes)
    across = all(value == reference for rows in processes for value in rows)
    return {'processes': len(processes), 'repeat': repeat,
            'queries': len(processes) * repeat,
            'in_process_deterministic': within,
            'cross_process_deterministic': across,
            'deterministic_case_rate': float(within and across),
            'matrix_sha256': hashes,
            'result_sha256': hashlib.sha256(reference).hexdigest(),
            'unique_ordered_results': len({value for rows in processes for value in rows})}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--harness', type=Path, required=True)
    parser.add_argument('--resources', type=Path, required=True)
    parser.add_argument('--cases', type=Path, required=True)
    parser.add_argument('--case-id', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--processes', type=int, default=20)
    parser.add_argument('--repeat', type=int, default=8)
    parser.add_argument('--warmup-queries', type=int, default=0,
                        help='Regression default has identical cold initial state in each process.')
    args = parser.parse_args(argv)
    try:
        output = confined(args.output)
        if args.processes < 2 or not 2 <= args.repeat <= 8:
            raise ValueError('stress requires multiple processes and repeats')
        cases = [json.loads(line) for line in args.cases.read_text(encoding='utf-8').splitlines() if line]
        selected = [case for case in cases if case['id'] == args.case_id]
        if len(selected) != 1:
            raise ValueError('case id must identify exactly one authored case')
        case = selected[0]
        output.parent.mkdir(parents=True, exist_ok=True)
        work = confined(output.parent / (output.stem + '-runs'))
        work.mkdir(exist_ok=True)
        corpus = confined(work / 'case.jsonl')
        corpus.write_text(json.dumps(case, ensure_ascii=False) + '\n', encoding='utf-8')
        matrix = []
        raw_hashes = []
        for ordinal in range(args.processes):
            result_path = confined(work / f'process-{ordinal:02d}.jsonl')
            command = [str(args.harness.resolve()), '--resources', str(args.resources.resolve()),
                       '--cases', str(corpus), '--language', case['language'],
                       '--output', str(result_path), '--repeat', str(args.repeat),
                       '--warmup-queries', str(args.warmup_queries),
                       '--work', str(confined(work / f'work-{ordinal:02d}'))]
            result = subprocess.run(command, capture_output=True, timeout=300, check=False)
            confined(work / f'process-{ordinal:02d}.stderr').write_bytes(result.stderr)
            if result.returncode or result.stderr:
                raise ValueError(f'harness process {ordinal} failed: exit {result.returncode}')
            raw = result_path.read_bytes()
            rows = [json.loads(line) for line in raw.decode('utf-8').splitlines() if line]
            meta = [row['run_meta'] for row in rows if 'run_meta' in row]
            actual = [row for row in rows if row.get('id') == args.case_id]
            if len(meta) != 1 or meta[0].get('result_schema') != 'epocanva-candidate-results-v3':
                raise ValueError('stress requires observable result schema v3')
            if len(actual) != 1 or actual[0]['status'] != 'ok':
                raise ValueError('missing or failed case')
            matrix.append([signature(row['candidates']) for row in actual[0]['repeats']])
            raw_hashes.append(hashlib.sha256(raw).hexdigest())
        report = assess(matrix, args.repeat)
        report.update(case_id=args.case_id, warmup_queries=args.warmup_queries,
                      harness_sha256=hashlib.sha256(args.harness.read_bytes()).hexdigest(),
                      corpus_sha256=hashlib.sha256(corpus.read_bytes()).hexdigest(),
                      run_file_sha256=raw_hashes)
        output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(json.dumps({key: report[key] for key in ('case_id', 'queries', 'deterministic_case_rate',
                                                      'unique_ordered_results', 'result_sha256')}))
        return 0 if report['deterministic_case_rate'] == 1.0 else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f'stress failed: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
