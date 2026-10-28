#!/usr/bin/env python3
"""Compare full wire-returned MATCH scores, row order, filtering and pagination.

Only fts_bm25_score_regression is recreated. No rounding is applied to scores:
their binary Python float representations are retained in the JSON report.
"""
import argparse
import hashlib
import json
from pathlib import Path

import pymysql


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', type=int, default=2881)
parser.add_argument('--revision', required=True)
parser.add_argument('--output', required=True)
parser.add_argument('--reference')
args = parser.parse_args()
report = {'revision': args.revision, 'cases': [], 'passed': False,
          'suite_source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
db = pymysql.connect(host='127.0.0.1', port=args.port, user='root', charset='utf8mb4',
                     autocommit=True, read_timeout=180)
try:
    with db.cursor() as cursor:
        cursor.execute('set ob_query_timeout=180000000')
        cursor.execute('drop database if exists fts_bm25_score_regression')
        cursor.execute('create database fts_bm25_score_regression')
        cursor.execute('use fts_bm25_score_regression')
        for tokenizer in ('beng', 'ik'):
            table = 'docs_' + tokenizer
            cursor.execute(f'create table {table}(id int primary key,body text)')
            rows = [(i, None if i % 89 == 0 else '' if i % 43 == 0 else
                     'database ' * (1 + i % 13) + ('audit ' * (1 + i % 7) if i % 9 else '') +
                     '全文索引 数据库 ' * (1 + i % 3)) for i in range(1, 2058)]
            for offset in range(0, len(rows), 200):
                cursor.executemany(f'insert into {table} values(%s,%s)', rows[offset:offset+200])
            cursor.execute(f'alter table {table} add fulltext index ft(body) with parser {tokenizer}')
            for query in ('database audit', '全文索引 数据库', 'missinguniqueword'):
                for count, offset, threshold in ((63, 0, 0), (65, 63, 0), (129, 0, .25),
                                                 (513, 127, 0), (2057, 0, 0)):
                    match = 'match(body) against(%s)'
                    sql = (f'select id,{match} as score from {table} where {match}>%s '
                           f'and mod(id,3)<>0 order by score desc,id limit {count} offset {offset}')
                    cursor.execute(sql, (query, query, threshold))
                    results = [[row_id, float(score).hex()] for row_id, score in cursor.fetchall()]
                    report['cases'].append({'name': f'{tokenizer}/{query}/{count}/{offset}/{threshold}',
                                            'rows': results})
    if args.reference:
        baseline = json.loads(Path(args.reference).read_text())['cases']
        assert report['cases'] == baseline, 'Complete scores or ordered row identities differ'
    report['result_sha256'] = hashlib.sha256(json.dumps(report['cases'], sort_keys=True).encode()).hexdigest()
    report['passed'] = True
finally:
    db.close()
    Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
print(json.dumps({k: v for k, v in report.items() if k != 'cases'}))
