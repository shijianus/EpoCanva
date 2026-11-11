#!/usr/bin/env python3
"""Compile verified local Mozc OSS rows and connections into a bounded binary."""
import argparse
import hashlib
import json
import pathlib
import struct


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--source-root',type=pathlib.Path,required=True)
    parser.add_argument('--output',type=pathlib.Path,required=True)
    args=parser.parse_args()
    repo=pathlib.Path(__file__).resolve().parents[2]
    out=args.output.resolve()
    if not out.is_relative_to(repo/'build') or out.exists():
        raise ValueError('output must be new beneath build/')
    pinned=json.loads((repo/'data/manifests/backend-ja-corpus.sources.json').read_text(encoding='utf-8'))['mozc']
    records_by_name={pathlib.PurePosixPath(row['upstream_path']).name:row for row in pinned['payload']}
    required=['connection_single_column.txt','id.def']+[f'dictionary{i:02}.txt' for i in range(10)]
    for name in required:
        path=args.source_root/name
        expected=records_by_name[name]
        source_bytes=path.read_bytes()
        if len(source_bytes)!=expected['bytes'] or hashlib.sha256(source_bytes).hexdigest()!=expected['sha256']:
            raise ValueError('Mozc input does not match pinned source manifest')
    connection=args.source_root/'connection_single_column.txt'
    with connection.open(encoding='utf-8') as stream:
        dimension=int(next(stream).strip())
        if not 1<=dimension<=4096:raise ValueError('invalid connection dimension')
        matrix=bytearray()
        for line in stream:
            cost=int(line.strip())
            if not -32768<=cost<=32767:raise ValueError('connection cost outside int16')
            matrix.extend(struct.pack('<h',cost))
        if len(matrix)!=dimension*dimension*2:raise ValueError('connection matrix size mismatch')
    records=[]
    count=0
    sources=[connection]
    for ordinal in range(10):
        path=args.source_root/f'dictionary{ordinal:02}.txt'
        if not path.exists():path=args.source_root/f'dictionary{ordinal:02}'
        sources.append(path)
        with path.open(encoding='utf-8') as stream:
            for line in stream:
                fields=line.rstrip('\r\n').split('\t')
                if len(fields)<5:raise ValueError('invalid dictionary row')
                reading,left,right,cost,surface=fields[:5]
                left,right,cost=int(left),int(right),int(cost)
                if not 0<=left<dimension or not 0<=right<dimension:raise ValueError('POS id outside matrix')
                a,b=reading.encode(),surface.encode()
                if not a or not b or len(a)>1024 or len(b)>1024 or '\0' in reading+surface:raise ValueError('invalid token')
                records.append(struct.pack('<HHiiI',len(a),len(b),left,right,cost & 0xffffffff)+a+b)
                count+=1
    out.parent.mkdir(parents=True,exist_ok=True)
    with out.open('xb') as stream:
        stream.write(b'EPCJA01\0')
        stream.write(struct.pack('<II',dimension,count))
        stream.write(matrix)
        for record in records:stream.write(record)
    content=out.read_bytes()
    manifest={'format':1,'source_revision':pinned['revision'],
              'connection_order':'previous_right_id * dimension + next_left_id',
              'dimension':dimension,'lexeme_rows':count,'output':{'path':out.name,'bytes':len(content),'sha256':hashlib.sha256(content).hexdigest()},
              'sources':[{'path':p.name,'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'bytes':p.stat().st_size} for p in sources],
              'scope':'Actual Mozc OSS lexical rows and connection costs; no claim of all Mozc rewriters, grammar quality or user learning.'}
    out.with_suffix('.manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'rows':count,'dimension':dimension,'bytes':len(content),'sha256':manifest['output']['sha256']}))

if __name__=='__main__':main()
