#!/usr/bin/env python3
"""Replay selected upstream public SQL cases against MiniOB and SQLite.

This is a local adaptation, not the OceanBase online judge. It implements
echo/sort/ensure directives used in the four selected upstream case files.
"""
import argparse
import json
import re
import sqlite3
import tempfile
import time
from pathlib import Path
from test_features import Server, parse_rows, equal_rows

CASES = ['primary-order-by', 'vectorized-order-by-limit',
         'vectorized-aggregation-and-group-by', 'dblab-hash-join', 'vectorized-basic']


def run(args):
    root = Path(__file__).resolve().parents[2]
    output = Path(args.results).resolve()
    output.mkdir(parents=True, exist_ok=True)
    results = []
    for name in CASES:
        directory = Path(tempfile.mkdtemp(prefix=name+'-', dir=output))
        server = Server(Path(args.observer).resolve(), directory,
                        Path(args.runtime_bin).resolve() if args.runtime_bin else None)
        oracle = sqlite3.connect(':memory:')
        try:
            for line in (root/'test/case/test'/f'{name}.test').read_text(encoding='utf-8').splitlines():
                line = line.strip()
                if not line or line.startswith('-- echo'): continue
                directive = None
                if line.startswith('--'):
                    directive, line = line[2:].strip().split(' ', 1)
                start = time.perf_counter()
                sql = ('explain '+line) if directive and directive.startswith('ensure:') else line
                response = server.sql(sql)
                error = None
                try:
                    if directive and directive.startswith('ensure:'):
                        target = directive.split(':')[1]
                        kind, _, count = target.partition('*')
                        key = {'hashjoin':'HASH_JOIN','nlj':'NESTED_LOOP_JOIN'}[kind]
                        passed = response.count(key) >= int(count or 1)
                    elif line.lower().startswith('select'):
                        expected = oracle.execute(line).fetchall()
                        actual = parse_rows(response)
                        if directive == 'sort':
                            # Compare multisets while tolerating MiniOB's two-decimal rendering.
                            def key(row):
                                return tuple(f'{float(v):.2f}' if isinstance(v,(int,float)) else str(v) for v in row)
                            actual.sort(key=lambda r: tuple(str(v) for v in r))
                            expected.sort(key=key)
                        passed = equal_rows(actual, expected)
                    elif line.lower().startswith('explain'):
                        passed = len(response.splitlines()) > 1
                    else:
                        passed = response == 'SUCCESS'
                        if not line.lower().startswith('set'):
                            oracle.execute(re.sub(r'\s+storage format\s*=\s*\w+', '', line, flags=re.I))
                except Exception as exc:
                    passed, error = False, str(exc)
                results.append(dict(case=name, sql=sql, passed=passed, response=response,
                                    error=error, elapsed_ms=round((time.perf_counter()-start)*1000,3)))
                if not passed: print('FAIL',name,sql,response[:200],error,flush=True)
        finally:
            server.close()
            oracle.close()
    summary = dict(total=len(results), passed=sum(x['passed'] for x in results),
                   failed=sum(not x['passed'] for x in results),
                   cases={name:sum(x['case']==name for x in results) for name in CASES},
                   note='Local SQLite adaptation of public upstream cases; not online judge results.')
    (output/'official-results.json').write_text(json.dumps(dict(summary=summary, cases=results),
                                                    ensure_ascii=False, indent=2),encoding='utf-8')
    print(json.dumps(summary),flush=True)
    return int(summary['failed'] != 0)

if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--observer',required=True)
    parser.add_argument('--runtime-bin')
    parser.add_argument('--results',default='official-results')
    raise SystemExit(run(parser.parse_args()))
