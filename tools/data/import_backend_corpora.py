#!/usr/bin/env python3
"""Prepare a bounded, attributed offline corpus from pinned local Tatoeba exports."""
import argparse
import array
import bz2
import hashlib
import heapq
import json
import pathlib
import tarfile

LANGUAGES = {'eng': 'en', 'cmn': 'zh', 'fra': 'fr', 'jpn': 'ja', 'kor': 'ko'}
LICENSE_URL = 'https://creativecommons.org/licenses/by/2.0/fr/'


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=pathlib.Path, required=True)
    parser.add_argument('--output-root', type=pathlib.Path, required=True)
    parser.add_argument('--max-per-language', type=int, default=100000)
    args = parser.parse_args()
    repo = pathlib.Path(__file__).resolve().parents[2]
    args.source_root = args.source_root.resolve()
    output = args.output_root.resolve()
    if not output.is_relative_to(repo / 'build') or output.exists():
        raise ValueError('output must be a new directory below build/')
    if not 2000 <= args.max_per_language <= 200000:
        raise ValueError('sample cap outside admitted range')
    pinned=json.loads((repo/'data/manifests/backend-ja-corpus.sources.json').read_text(encoding='utf-8'))['tatoeba']
    for entry in pinned['snapshots']+[pinned['grouping_export']]:
        candidate=args.source_root/pathlib.PurePosixPath(entry['path']).name
        if candidate.stat().st_size!=entry['bytes'] or sha(candidate)!=entry['sha256']:
            raise ValueError('Tatoeba input does not match the pinned source manifest')
    base = args.source_root / 'sentences_base.tar.bz2'
    parent = array.array('I', [0])
    base_rows = 0
    with tarfile.open(base, 'r|bz2') as archive:
        entry = next(iter(archive))
        if not entry.isfile() or entry.name != 'sentences_base.csv':
            raise ValueError('unexpected attribution grouping archive')
        with archive.extractfile(entry) as stream:
            for raw in stream:
                fields = raw.rstrip(b'\r\n').split(b'\t')
                if len(fields) != 2:
                    raise ValueError('invalid base relation')
                sentence = int(fields[0])
                if sentence > 30000000:
                    raise ValueError('base record exceeds declared memory bound')
                if sentence >= len(parent):
                    parent.extend(array.array('I', [0]) * (sentence + 65536 - len(parent)))
                if fields[1] != b'\\N':
                    related = int(fields[1])
                    if related > 30000000 or related < 0:
                        raise ValueError('base relation outside bound')
                    parent[sentence] = related or sentence
                base_rows += 1

    def group(sentence):
        visited = set()
        while sentence < len(parent) and parent[sentence]:
            if sentence in visited:
                return None
            visited.add(sentence)
            target = parent[sentence]
            if target == sentence:
                return sentence
            sentence = target
        return None

    selected = {}
    sources = []
    for source_language, language in LANGUAGES.items():
        path = args.source_root / f'{source_language}_sentences_detailed.tsv.bz2'
        sources.append({'path': str(path.relative_to(repo)), 'sha256': sha(path), 'bytes': path.stat().st_size,
                        'url': f'https://downloads.tatoeba.org/exports/per_language/{source_language}/{source_language}_sentences_detailed.tsv.bz2',
                        'license': 'CC-BY-2.0-FR', 'license_url': LICENSE_URL})
        heap = []
        counts = {'read': 0, 'unresolved_group': 0, 'no_author': 0, 'invalid_text': 0}
        with bz2.open(path, 'rt', encoding='utf-8', errors='strict', newline='') as stream:
            for line in stream:
                fields = line.rstrip('\r\n').split('\t')
                if len(fields) != 6:
                    raise ValueError('detailed export must preserve six fields')
                sentence_id, code, text, author, _, _ = fields
                counts['read'] += 1
                if code != source_language:
                    raise ValueError('language mismatch in attributed source')
                if not author or author == '\\N':
                    counts['no_author'] += 1
                    continue
                raw = text.encode('utf-8')
                if not raw or len(raw) > 4096 or any(ord(ch) < 32 or 127 <= ord(ch) <= 159 for ch in text):
                    counts['invalid_text'] += 1
                    continue
                root = group(int(sentence_id))
                if root is None:
                    counts['unresolved_group'] += 1
                    continue
                record = {'id': 'tatoeba-' + sentence_id, 'language': language, 'text': text,
                          'source': 'tatoeba-detailed-snapshot', 'license': 'CC-BY-2.0-FR',
                          'document_id': 'tatoeba-base-' + str(root),
                          'attribution': {'author': author, 'sentence_url': 'https://tatoeba.org/sentences/show/' + sentence_id,
                                          'license_url': LICENSE_URL}}
                priority = int.from_bytes(hashlib.sha256(('epocanva-corpus-v1/' + language + '/' + sentence_id).encode()).digest()[:8], 'big')
                item = (-priority, int(sentence_id), record)
                if len(heap) < args.max_per_language:
                    heapq.heappush(heap, item)
                elif item[:2] > heap[0][:2]:
                    heapq.heapreplace(heap, item)
        selected[language] = ([item[2] for item in sorted(heap, key=lambda item: item[1])], counts)
    output.mkdir(parents=True)
    reports = {}
    for language, (records, counts) in selected.items():
        destination = output / f'{language}.jsonl'
        with destination.open('w', encoding='utf-8', newline='\n') as stream:
            for record in records:
                stream.write(json.dumps(record, ensure_ascii=False, sort_keys=True) + '\n')
        reports[language] = {**counts, 'selected': len(records), 'sha256': sha(destination),
                             'bytes': destination.stat().st_size, 'path': destination.name}
    manifest = {'format': 1, 'sources': sources, 'base': {'sha256': sha(base), 'bytes': base.stat().st_size, 'rows': base_rows},
                'selection': 'deterministic smallest SHA256 prefix by language/id, attributed records with resolved base family only',
                'grouping_limit': 'base provenance families are grouped; arbitrary semantic paraphrases and unrecorded direct links are not proven disjoint',
                'max_per_language': args.max_per_language, 'outputs': reports,
                'attribution': 'Each retained sentence preserves author, sentence URL and CC-BY-2.0-FR license reference; derived models require attribution and source notice.'}
    (output / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({language: data['selected'] for language, data in reports.items()}, sort_keys=True))


if __name__ == '__main__':
    main()
