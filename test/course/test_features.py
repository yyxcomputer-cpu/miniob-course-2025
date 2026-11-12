#!/usr/bin/env python3
"""End-to-end tests against a real MiniOB server, using SQLite as an oracle.

No third-party Python modules are required. Each run uses a new data directory.
"""
import argparse
import collections
import csv
import json
import math
import os
from pathlib import Path
import random
import socket
import sqlite3
import subprocess
import tempfile
import time


class Server:
    def __init__(self, observer, directory, runtime_bin=None):
        self.directory = directory
        directory.mkdir(parents=True, exist_ok=True)
        config = directory / 'observer.ini'
        config.write_text('[LOG]\nLOG_FILE_NAME=observer.log\nLOG_FILE_LEVEL=1\nLOG_CONSOLE_LEVEL=1\n')
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            self.port = probe.getsockname()[1]
        env = dict(os.environ)
        if runtime_bin:
            env['PATH'] = str(runtime_bin) + os.pathsep + env.get('PATH', '')
        self.console = (directory / 'console.log').open('wb')
        self.process = subprocess.Popen(
            [str(observer), '-f', str(config), '-p', str(self.port)],
            cwd=directory, env=env, stdout=self.console, stderr=subprocess.STDOUT)
        for _ in range(100):
            if self.process.poll() is not None:
                raise RuntimeError('Server exited: ' + str(self.process.returncode))
            try:
                self.socket = socket.create_connection(('127.0.0.1', self.port), timeout=1)
                self.socket.settimeout(30)
                break
            except OSError:
                time.sleep(0.1)
        else:
            self.close()
            raise RuntimeError('Server did not start')

    def sql(self, sql):
        self.socket.sendall(sql.encode('utf-8') + b'\0')
        data = bytearray()
        while b'\0' not in data:
            part = self.socket.recv(65536)
            if not part:
                raise RuntimeError('Server closed connection: ' + sql)
            data.extend(part)
            if len(data) > 64 * 1024 * 1024:
                raise RuntimeError('Response exceeds test limit')
        return data.split(b'\0', 1)[0].decode('utf-8', errors='replace').strip()

    def close(self):
        if hasattr(self, 'socket'):
            self.socket.close()
        if self.process.poll() is None:
            if os.name == 'nt':
                self.process.terminate()
            else:
                self.process.send_signal(__import__('signal').SIGTERM)
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.console.close()


def parse_rows(response):
    if response == 'FAILURE' or response.startswith('FAILURE\n'):
        raise ValueError(response)
    lines = response.splitlines()
    return [[cell.strip() for cell in line.split('|')] for line in lines[1:]]


def equal_rows(actual, expected):
    if len(actual) != len(expected):
        return False
    for got, wanted in zip(actual, expected):
        if len(got) != len(wanted):
            return False
        for a, b in zip(got, wanted):
            if isinstance(b, (int, float)):
                try:
                    if not math.isclose(float(a), float(b), rel_tol=0, abs_tol=0.011):
                        return False
                except ValueError:
                    return False
            elif a != str(b):
                return False
    return True


def run(args):
    project = Path(__file__).resolve().parents[2]
    results_dir = Path(args.results).resolve()
    results_dir.mkdir(parents=True, exist_ok=True)
    run_dir = Path(tempfile.mkdtemp(prefix='run-', dir=results_dir))
    server = Server(Path(args.observer).resolve(), run_dir,
                    Path(args.runtime_bin).resolve() if args.runtime_bin else None)
    oracle = sqlite3.connect(':memory:')
    results = []

    def command(sql, oracle_sql=None):
        response = server.sql(sql)
        if response != 'SUCCESS':
            raise AssertionError(f'Setup failed: {sql}: {response}')
        if oracle_sql is not None:
            oracle.execute(oracle_sql)

    def check(category, sql, expected=None, failure=False, unordered=False, contains=None):
        start = time.perf_counter()
        response = server.sql(sql)
        error = None
        try:
            if failure:
                ok = response == 'FAILURE' or response.startswith('FAILURE')
                if not ok:
                    # Error messages may carry an RC name before/after the state text.
                    ok = any(x in response for x in ['INVALID_ARGUMENT', 'SCHEMA_', 'SQL_SYNTAX', 'Load stopped'])
            elif contains is not None:
                ok = contains in response
            else:
                if expected is None:
                    expected = oracle.execute(sql).fetchall()
                actual = parse_rows(response)
                if unordered:
                    actual = sorted(actual, key=lambda row: tuple(str(v) for v in row))
                    expected = sorted(expected, key=lambda row: tuple(str(v) for v in row))
                ok = equal_rows(actual, expected)
        except Exception as exc:
            ok = False
            error = str(exc)
        result = dict(category=category, sql=sql, passed=ok,
                      elapsed_ms=round((time.perf_counter() - start) * 1000, 3),
                      expected=expected, response=response, error=error)
        results.append(result)
        if not ok:
            print('FAIL', category, sql, repr(response[:350]), 'expected=', str(expected)[:200], flush=True)

    try:
        rng = random.Random(20251109)
        records = [(i, rng.randrange(-9, 10), rng.randrange(-80, 160) / 4, 'n' + str(rng.randrange(7)))
                   for i in range(1, 161)]
        for fmt, mode in [('row', 'tuple_iterator'), ('pax', 'chunk_iterator')]:
            table = 'students_' + fmt
            empty = 'empty_' + fmt
            command(f"set execution_mode='{mode}';")
            for name in [table, empty]:
                command(f'create table {name}(id int, grp int, score float, name char(16)) storage format={fmt};',
                        f'create table {name}(id int, grp int, score real, name text);')
            for record in records:
                sql = f"insert into {table} values({record[0]},{record[1]},{record[2]},'{record[3]}');"
                command(sql, sql)
            base = [
                f'select * from {table} order by id;',
                f'select * from {table} order by score desc, id;',
                f'select name from {table} order by grp, score desc, id;',
                f'select id,score from {table} where grp>0 and score<20.0 order by score, id;',
                f'select id from {table} order by score*2.0+1.0 desc,id;',
                f'select * from {empty} order by score;',
                f'select * from {table} where id>10000 order by id;',
            ]
            for sql in base:
                check(fmt + '-sort', sql)
            for _ in range(70):
                key = rng.choice(['grp', 'score', 'name'])
                direction = rng.choice(['asc', 'desc'])
                count, offset = rng.randrange(0, 31), rng.randrange(0, 190)
                sql = f'select id,name from {table} order by {key} {direction},id limit {count} offset {offset};'
                check(fmt + '-limit-random', sql)
            check(fmt + '-limit', f'select id from {table} order by id limit 5,12;',
                  oracle.execute(f'select id from {table} order by id limit 12 offset 5;').fetchall())
            check(fmt + '-limit', f'select id from {table} limit 0;')
            check(fmt + '-limit', f'select id from {table} limit 4;')
            for sql in [
                f'select count(*),count(name),avg(score),min(score),max(score),sum(score) from {table};',
                f'select grp,count(*),sum(score),avg(score),min(score),max(score) from {table} group by grp order by grp;',
                f'select name,grp,count(*) from {table} group by name,grp order by name,grp;',
                f'select grp,sum(score) from {table} group by grp order by sum(score) desc,grp limit 5;',
                f'select grp from {table} group by grp order by grp;',
                f'select min(name),max(name) from {table};',
                f'select count(*) from {empty};',
            ]:
                check(fmt + '-aggregate', sql)
            for sql in [
                f'select * from {table} order by missing;',
                f'select * from {table} order by nowhere.id;',
                f'select * from {table} order by id limit -1;',
                f'select * from {table} limit 2 offset -2;',
                f'select * from {table} order by;',
                f'select * from {table} order by *;',
                f'select avg(name) from {table};',
            ]:
                check(fmt + '-invalid', sql, failure=True)
            check(fmt + '-explain', f'explain select id from {table} order by score limit 2;',
                  contains='ORDER_BY_VEC' if fmt == 'pax' else 'ORDER_BY')
            for sql in [
                f'select count(*),sum(1),avg(1),min(1),max(1) from {table};',
                f'select grp,count(*),sum(1) from {table} group by grp order by grp;',
                f'select grp+id+grp+id,count(*) from {table} group by grp+id order by grp+id;',
                f'select score,count(*) from {table} group by score order by score;',
                f'select grp,count(*) from {empty} group by grp order by grp;',
                f'select id from {table} order by 1, id;',
            ]:
                check(fmt+'-edge',sql)
            for sql in [f'select sum(*) from {table};',f'select min(*) from {table};',
                        f'select avg() from {table};',f'select count(id,grp) from {table};',
                        f'select grp,count(*) from {table};']:
                check(fmt+'-invalid-extra',sql,failure=True)

        command("set execution_mode='tuple_iterator';")
        for name in ['left_t', 'right_t', 'third_t', 'empty_t']:
            command(f'create table {name}(id int, tag char(16), amount int);',
                    f'create table {name}(id int, tag text, amount int);')
        for name, data in [
            ('left_t', [(1,'aa',1),(1,'aa',2),(2,'bb',3),(3,'aa',4),(-1,'zz',5)]),
            ('right_t', [(1,'aa',10),(1,'aa',20),(2,'cc',30),(4,'bb',40),(-1,'zz',50)]),
            ('third_t', [(1,'xx',100),(2,'yy',200),(-1,'zz',300)])]:
            for a,b,c in data:
                sql=f"insert into {name} values({a},'{b}',{c});"
                command(sql,sql)
        join_queries=[
            'select * from left_t inner join right_t on left_t.id=right_t.id order by left_t.amount,right_t.amount;',
            'select * from left_t inner join right_t on right_t.id=left_t.id and left_t.tag=right_t.tag order by left_t.amount,right_t.amount;',
            'select left_t.amount,right_t.amount from left_t,right_t where left_t.id=right_t.id and right_t.amount>15 order by left_t.amount,right_t.amount;',
            'select * from left_t inner join right_t on left_t.tag=right_t.tag order by left_t.amount,right_t.amount;',
            'select * from left_t inner join empty_t on left_t.id=empty_t.id order by left_t.amount;',
            'select * from empty_t inner join left_t on left_t.id=empty_t.id order by left_t.amount;',
            'select * from left_t inner join right_t on left_t.id<right_t.id order by left_t.amount,right_t.amount;',
            'select * from left_t inner join right_t on left_t.id=right_t.id inner join third_t on left_t.id=third_t.id order by left_t.amount,right_t.amount,third_t.amount;',
        ]
        for enabled in [0,1]:
            command(f'set hash_join={enabled};')
            for sql in join_queries:
                check('join-' + str(enabled), sql)
        check('join-explain', 'explain select * from left_t inner join right_t on left_t.id=right_t.id;', contains='HASH_JOIN')
        check('join-explain', 'explain select * from left_t inner join right_t on left_t.id<right_t.id;', contains='NESTED_LOOP_JOIN')
        command('set use_cascade=1;')
        check('cascade-fallback', 'select id from students_row order by id desc limit 4;')
        command('set use_cascade=0;')

        # More than one 8192-row chunk, with an OFFSET crossing a chunk boundary.
        large = [(i, i % 23, (i % 131)/4, 'v'+str(i%5)) for i in range(10030,10030-9001,-1)]
        for fmt, mode in [('row','tuple_iterator'), ('pax','chunk_iterator')]:
            table='bulk_'+fmt
            command(f'create table {table}(id int,grp int,score float,name char(16)) storage format={fmt};',
                    f'create table {table}(id int,grp int,score real,name text);')
            path=run_dir/(table+'.csv')
            with path.open('w',newline='') as handle:
                csv.writer(handle).writerows(large)
            check(fmt+'-load', f"load data infile '{path.name}' into table {table};", contains='SUCCESS')
            oracle.executemany(f'insert into {table} values(?,?,?,?)',large)
            command(f"set execution_mode='{mode}';")
            for sql in [
                f'select id,name from {table} order by id;',
                f'select id from {table} order by id desc limit 20 offset 8187;',
                f'select id from {table} limit 20 offset 8187;',
                f'select grp,count(*),sum(score) from {table} group by grp order by grp;',
            ]:
                check(fmt+'-cross-chunk',sql)
            # Import errors must be reported instead of silently returning SUCCESS.
            (run_dir/'invalid.csv').write_text('oops,2,3.0,a\n')
            check(fmt+'-load-invalid',f"load data infile 'invalid.csv' into table {table};",failure=True)
        command("set execution_mode='tuple_iterator';")
        command('create table csv_quotes(id int,name char(32));')
        (run_dir/'quotes.csv').write_text('1,"hello,world"\n2,"he said ""yes"""\n')
        check('csv-quoted','load data infile \'quotes.csv\' into table csv_quotes;',contains='SUCCESS')
        check('csv-quoted','select * from csv_quotes order by id;',[(1,'hello,world'),(2,'he said "yes"')])
        command('create table limits_i(id int);', 'create table limits_i(id int);')
        for i in [2147483647,-2147483647,0,1,-1]:
            command(f'insert into limits_i values({i});',f'insert into limits_i values({i});')
        check('integer-edge','select * from limits_i order by id;')
        check('integer-edge','select min(id),max(id) from limits_i;')
        for name in ['float_left','float_right']:
            command(f'create table {name}(id float);',f'create table {name}(id real);')
            for value in [1.0,2.5,-0.5]:
                command(f'insert into {name} values({value});',f'insert into {name} values({value});')
        check('float-join-fallback','select * from float_left inner join float_right on float_left.id=float_right.id order by float_left.id;')
        check('float-join-fallback','explain select * from float_left inner join float_right on float_left.id=float_right.id;',contains='NESTED_LOOP_JOIN')
        (run_dir/'custom.csv').write_text("3;#semi;colon#\n4;#plain#\n")
        check('csv-custom',"load data infile 'custom.csv' into table csv_quotes fields terminated by ';' enclosed by '#';",contains='SUCCESS')
        check('csv-custom','select * from csv_quotes where id>=3 order by id;',[(3,'semi;colon'),(4,'plain')])
    finally:
        server.close()
        counts=collections.Counter(r['category'] for r in results)
        summary=dict(total=len(results),passed=sum(r['passed'] for r in results),
                     failed=sum(not r['passed'] for r in results), categories=dict(counts),
                     seed=20251109, platform=os.name, observer=str(Path(args.observer).resolve()))
        (results_dir/'results.json').write_text(json.dumps(dict(summary=summary,cases=results),ensure_ascii=False,indent=2),encoding='utf-8')
        print(json.dumps(summary,ensure_ascii=False),flush=True)
    return 0 if all(r['passed'] for r in results) else 1


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--observer',required=True)
    parser.add_argument('--runtime-bin')
    parser.add_argument('--results',default='test-results')
    raise SystemExit(run(parser.parse_args()))
