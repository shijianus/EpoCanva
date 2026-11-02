#!/usr/bin/env python3
"""Stage the explicitly reviewed Chinese backend inputs, without network access."""
import argparse
import collections
import hashlib
import json
import pathlib
import shutil

SOURCE_HASHES = {
    'terra/terra_pinyin.dict.yaml': '2f881a239e09a61e5993e79257655f98f81c3789dc8aeea5f33477f710345b27',
    'bopomofo/zhuyin.yaml': '77931ab77952fdb2535f8addebc0918453e0220a50ca176380971203f14c48be',
    'terra/LICENSE': 'da7eabb7bafdf7d3ae5e9f223aa5bdc1eece45ac569dc21b3b037520b4464768',
    'bopomofo/LICENSE': 'da7eabb7bafdf7d3ae5e9f223aa5bdc1eece45ac569dc21b3b037520b4464768',
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=pathlib.Path, required=True)
    parser.add_argument('--output-root', type=pathlib.Path, required=True)
    parser.add_argument('--corpus', type=pathlib.Path, required=True)
    args = parser.parse_args()
    repo = pathlib.Path(__file__).resolve().parents[2]
    target = args.output_root.resolve()
    if not target.is_relative_to(repo / 'build') or target.exists():
        raise ValueError('output must be new and below build/')
    sources = {}
    for relative, expected in SOURCE_HASHES.items():
        data = (args.source_root / relative).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError('pinned Chinese input hash mismatch')
        sources[relative] = data
    original = sources['terra/terra_pinyin.dict.yaml'].decode('utf-8')
    if original.count('name: terra_pinyin\n') != 1 or original.count('use_preset_vocabulary: true\n') != 1:
        raise ValueError('unexpected upstream dictionary header')
    derived = original.replace('name: terra_pinyin\n', 'name: epocanva_backend_terra\n', 1)
    derived = derived.replace('use_preset_vocabulary: true\n', 'use_preset_vocabulary: false\n', 1)
    if 'import_tables:' in derived:
        raise ValueError('unexpected dictionary dependency')
    counts = collections.Counter()
    with args.corpus.open(encoding='utf-8') as stream:
        for line in stream:
            record = json.loads(line)
            if record['language'] != 'zh' or not record.get('attribution'):
                raise ValueError('Chinese corpus requires language and attribution')
            counts.update(record['text'])
    header, body = derived.split('...\n', 1)
    rows = []
    for line in body.splitlines():
        if not line or line.startswith('#'):
            rows.append(line)
            continue
        parts = line.split('\t')
        if len(parts) < 2:
            raise ValueError('unexpected dictionary record')
        weight = max(1, counts.get(parts[0], 0))
        if len(parts) > 2 and parts[2].endswith('%'):
            weight = max(1, round(weight * float(parts[2][:-1]) / 100))
        rows.append(parts[0] + '\t' + parts[1] + '\t' + str(weight))
    derived = header + '...\n' + '\n'.join(rows) + '\n'
    (target / 'shared').mkdir(parents=True)
    (target / 'user').mkdir()
    (target / 'logs').mkdir()
    (target / 'notices').mkdir()
    (target / 'shared/epocanva_backend_terra.dict.yaml').write_text(derived, encoding='utf-8', newline='\n')
    (target / 'shared/zhuyin.yaml').write_bytes(sources['bopomofo/zhuyin.yaml'])
    for name in ('epocanva_backend_pinyin', 'epocanva_backend_zhuyin'):
        shutil.copyfile(repo / f'data/backend/zh/{name}.schema.yaml', target / f'shared/{name}.schema.yaml')
    (target / 'shared/default.yaml').write_text('config_version: "epocanva-backend-v1"\nschema_list:\n  - schema: epocanva_backend_pinyin\n  - schema: epocanva_backend_zhuyin\nmenu:\n  page_size: 16\n', encoding='utf-8', newline='\n')
    (target / 'notices/rime-terra-LGPL3.txt').write_bytes(sources['terra/LICENSE'])
    (target / 'notices/rime-bopomofo-LGPL3.txt').write_bytes(sources['bopomofo/LICENSE'])
    (target / 'notices/NOTICE.txt').write_text(
        'Terra Pinyin data from rime/rime-terra-pinyin, commit 723e51bc266cf9464530c1ddedb856aa18e3da34.\n'
        'Its dictionary header attributes CC-CEDICT, MDBG, CC BY-SA 3.0: https://creativecommons.org/licenses/by-sa/3.0/\n'
        'The repository LICENSE is LGPL v3. These notices do not relicense the referenced dictionary data.\n'
        'Zhuyin rules from rime/rime-bopomofo commit 6085c9a38a4a728047862b33d67eee18aa86f3b9, LGPL v3.\n'
        'Dictionary modifications: renamed resource, disabled preset vocabulary, retained readings/surfaces, generated character weights from attributed Tatoeba cmn corpus with floor-one smoothing.\n'
        'Tatoeba-derived weights: CC-BY-2.0-FR; exact input retains sentence authors and URLs in prepared corpus.\n'
        'Project-authored minimal schemas remove custom phrases, reverse lookup, simplifier and implicit history learning.\n'
        'This engineering dataset does not assert public-distribution legal clearance or full conversion quality.\n', encoding='utf-8', newline='\n')
    records = []
    for path in sorted(target.rglob('*')):
        if path.is_file():
            data = path.read_bytes()
            records.append({'path': path.relative_to(target).as_posix(), 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)})
    manifest = {'format': 1, 'input_sha256': SOURCE_HASHES, 'files': records,
                'dictionary_readings_and_surfaces_retained': True, 'preset_vocabulary': False,
                'corpus_sha256': hashlib.sha256(args.corpus.read_bytes()).hexdigest(),
                'weighting': 'corpus-character-count with existing reading percentages and floor-one smoothing',
                'personal_learning': False, 'public_distribution_authorized': False}
    (target / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print('Staged actual pinned Chinese dictionary/rules; rows preserved, user learning disabled.')


if __name__ == '__main__':
    main()
