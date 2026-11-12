#!/usr/bin/env python3
"""Small reproducible, single-client elapsed-time experiment (not contest score)."""
import argparse
import csv
import json
import platform
import statistics
import tempfile
import time
from pathlib import Path
from test_features import Server, parse_rows

def run(args):
    out=Path(args.results).resolve()
    out.mkdir(parents=True,exist_ok=True)
    directory=Path(tempfile.mkdtemp(prefix='benchmark-',dir=out))
    server=Server(Path(args.observer).resolve(),directory,
                  Path(args.runtime_bin).resolve() if args.runtime_bin else None)
    results=[]
    def sql(text):
        response=server.sql(text)
        if response.startswith('FAILURE'): raise RuntimeError(text+': '+response)
        return response
    def measure(label,query,expected_rows=None,expected_count=None):
        response=sql(query) # warm up
        times=[]
        for _ in range(5):
            start=time.perf_counter()
            response=sql(query)
            times.append((time.perf_counter()-start)*1000)
        rows=parse_rows(response)
        if expected_rows is not None: assert len(rows)==expected_rows,(label,len(rows))
        if expected_count is not None: assert int(rows[0][0])==expected_count,(label,rows)
        results.append(dict(label=label,sql=query,samples_ms=[round(t,3) for t in times],
                            median_ms=round(statistics.median(times),3),output_rows=len(rows)))
    try:
        for n in [1000,10000]:
            table=f'sort_{n}'
            sql(f'create table {table}(id int,score int);')
            with (directory/f'{table}.csv').open('w',newline='') as f:
                csv.writer(f).writerows((i,(i*7919)%10007) for i in range(n,0,-1))
            assert 'SUCCESS' in sql(f"load data infile '{table}.csv' into table {table};")
            measure(f'ORDER BY / {n} rows',f'select id from {table} order by score,id;',expected_rows=n)
        for n in [500,2000]:
            for side in ['l','r']:
                table=f'join_{side}_{n}'
                sql(f'create table {table}(id int);')
                (directory/f'{table}.csv').write_text(''.join(str(i)+'\n' for i in range(n)))
                assert 'SUCCESS' in sql(f"load data infile '{table}.csv' into table {table};")
            for enabled in [0,1]:
                sql(f'set hash_join={enabled};')
                measure(f'{"Hash Join" if enabled else "Nested Loop"} / {n} x {n}',
                        f'select count(*) from join_l_{n} inner join join_r_{n} on join_l_{n}.id=join_r_{n}.id;',
                        expected_count=n)
    finally: server.close()
    report=dict(environment=platform.platform(),repeats=5,warmups=1,
                timing='Client wall time includes server execution, TCP and response decoding; one local process, no concurrent workload.',
                results=results)
    (out/'benchmark.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False),flush=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--observer',required=True)
    parser.add_argument('--runtime-bin')
    parser.add_argument('--results',default='benchmark-results')
    run(parser.parse_args())
