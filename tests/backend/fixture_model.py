import collections
import hashlib
import pathlib
import sys


def identity(language, token):
    value = 14695981039346656037
    for byte in ('epocanva.prediction.lexeme/v1/' + language + '/' + token).encode('utf-8'):
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value or 1


root = pathlib.Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
sentences = [
    ('en', ['hello', 'world']), ('en', ['hello', 'friend']),
    ('en', ['good', 'morning']), ('en', ['good', 'morning']),
    ('en', ['good', 'morning']), ('en', ['walk', 'walking']),
    ('en', ['be', 'right', 'back']), ('en', ['cat', 'cats']),
    ('fr', ['bonjour', 'monde']), ('fr', ['café', 'chaud']),
    ('ko', ['안녕', '세계']), ('ja', ['こんにちは', '世界']),
    ('zh', ['你好', '世界']),
]
lexemes = {}
unigram = collections.Counter()
bigram = collections.Counter()
trigram = collections.Counter()
for language, tokens in sentences:
    ids = []
    for token in tokens:
        token_id = identity(language, token)
        lexemes[token_id] = (language, token)
        ids.append(token_id)
    ids.append(0)
    unigram.update(ids)
    bigram.update(zip(ids, ids[1:]))
    trigram.update(zip(ids, ids[1:], ids[2:]))
rows = [f'L\t{i}\t{language}\tword\t{text}\n' for i, (language, text) in sorted(lexemes.items())]
rows += [f'U\t{i}\t{n}\n' for i, n in sorted(unigram.items())]
rows += [f'B\t{a}\t{b}\t{n}\n' for (a, b), n in sorted(bigram.items())]
rows += [f'T\t{a}\t{b}\t{c}\t{n}\n' for (a, b, c), n in sorted(trigram.items())]
payload = ''.join(rows).encode('utf-8')
header = ('EPOCANVA_PREDICTION_TSV\t1\nid_scheme\tfnv1a64-language-normalized-v1\n'
          'normalization\tascii-lower-utf8-v1\nsource\ttest-fixture\nsource_sha256\t' +
          hashlib.sha256(repr(sentences).encode()).hexdigest() + '\npayload_sha256\t' +
          hashlib.sha256(payload).hexdigest() + f'\nlexemes\t{len(lexemes)}\nunigram_total\t{sum(unigram.values())}\n'
          f'bigram_total\t{sum(bigram.values())}\ntrigram_total\t{sum(trigram.values())}\ndata\n')
model = header.encode('utf-8') + payload
(root / 'model.tsv').write_bytes(model)
(root / 'model.sha256').write_text(hashlib.sha256(model).hexdigest(), encoding='ascii')
