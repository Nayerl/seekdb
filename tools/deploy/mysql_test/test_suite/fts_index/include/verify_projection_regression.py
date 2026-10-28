#!/usr/bin/env python3
"""Compare ranked MATCH results across batch boundaries and repeated rescans."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import time

import pymysql


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=2881)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--reference')
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    report = {'revision': args.revision, 'started': time.time(), 'cases': [],
              'suite_source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    db = pymysql.connect(host='127.0.0.1', port=args.port, user='root',
                         password=os.environ.get('SEEKDB_TEST_PASSWORD', ''),
                         charset='utf8mb4', autocommit=True, read_timeout=180)
    try:
        with db.cursor() as cur:
            cur.execute('SET ob_query_timeout=180000000')
            cur.execute('DROP DATABASE IF EXISTS fts_projection_regression')
            cur.execute('CREATE DATABASE fts_projection_regression')
            cur.execute('USE fts_projection_regression')

            def capture(name, sql, values=()):
                cur.execute(sql, values)
                # ROUND in SQL fixes a tolerance for floating-point scores; row
                # membership and ordering remain exact baseline comparisons.
                rows = [[str(v) if not isinstance(v, (int, str, type(None))) else v
                         for v in row] for row in cur.fetchall()]
                report['cases'].append({'name': name, 'rows': rows})

            documents = [
                (i, f'Document {i} 全文索引',
                 None if i % 89 == 0 else '' if i % 43 == 0 else
                 'database ' * (1 + i % 13) +
                 ('audit ' * (1 + i % 7) if i % 9 else '') +
                 '全文索引 分词器 数据库 ' * (1 + i % 3))
                for i in range(1, 2058)
            ]
            for tokenizer in ('beng', 'ik'):
                table = 'docs_' + tokenizer
                cur.execute(f'CREATE TABLE {table}(id INT PRIMARY KEY, title VARCHAR(100), body TEXT)')
                for offset in range(0, len(documents), 200):
                    cur.executemany(f'INSERT INTO {table} VALUES(%s,%s,%s)', documents[offset:offset+200])
                cur.execute(f'ALTER TABLE {table} ADD FULLTEXT INDEX ft(title,body) WITH PARSER {tokenizer}')
                for query_index, (query, mode) in enumerate((
                    ('database audit', ''), ('全文索引 分词器', ''),
                    ('missing_unique_term', ''), ('+database -audit', 'IN BOOLEAN MODE'),
                )):
                    match = f'MATCH(title,body) AGAINST(%s {mode})'
                    base = f'SELECT id,ROUND({match},9) AS score FROM {table} WHERE {match}>0'
                    for limit in (0, 1, 63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512, 513, 1024, 2057):
                        capture(f'{tokenizer}/{query_index}/limit/{limit}',
                                base + f' ORDER BY score DESC,id LIMIT {limit}', (query, query))
                    capture(f'{tokenizer}/{query_index}/offset',
                            base + ' ORDER BY score DESC,id LIMIT 129 OFFSET 63', (query, query))
                    capture(f'{tokenizer}/{query_index}/aggregate',
                            f'SELECT COUNT(*),SUM(id),ROUND(SUM({match}),9) FROM {table} WHERE {match}>0',
                            (query, query))
                # Re-execute one prepared plan with different search terms,
                # including an empty result between nonempty result batches.
                prepared_sql = (f'SELECT id,ROUND(MATCH(title,body) AGAINST(?),9) AS score '
                                f'FROM {table} WHERE MATCH(title,body) AGAINST(?)>0 '
                                'ORDER BY score DESC,id LIMIT 129')
                cur.execute('PREPARE ranked_query FROM %s', (prepared_sql,))
                for index, term in enumerate(('database', 'audit', 'missing_unique_term', 'database')):
                    cur.execute('SET @ranked_term=%s', (term,))
                    capture(f'{tokenizer}/prepared/{index}',
                            'EXECUTE ranked_query USING @ranked_term,@ranked_term')
                cur.execute('DEALLOCATE PREPARE ranked_query')
                cur.execute(f"UPDATE {table} SET body='fresh lighthouse database' WHERE id BETWEEN 60 AND 70")
                cur.execute(f'DELETE FROM {table} WHERE id BETWEEN 250 AND 260')
                capture(f'{tokenizer}/after-dml',
                        f'SELECT id,ROUND(MATCH(title,body) AGAINST(\'fresh database\'),9) AS score '
                        f'FROM {table} WHERE MATCH(title,body) AGAINST(\'fresh database\') '
                        'ORDER BY score DESC,id LIMIT 513')
            report['result_sha256'] = hashlib.sha256(json.dumps(report['cases'], sort_keys=True).encode()).hexdigest()
            if args.reference:
                frozen = json.loads(Path(args.reference).read_text())['cases']
                report['differences'] = [case['name'] for case, old in zip(report['cases'], frozen) if case != old]
                assert not report['differences'] and len(report['cases']) == len(frozen), report['differences']
            report['passed'] = True
            print(json.dumps({key: value for key, value in report.items() if key != 'cases'}), flush=True)
    except Exception as error:
        report.update(passed=False, failure=str(error))
        raise
    finally:
        report['finished'] = time.time()
        Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        db.close()


if __name__ == '__main__':
    main()
