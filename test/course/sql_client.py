#!/usr/bin/env python3
"""Minimal NUL-terminated MiniOB TCP client, Python standard library only."""
import argparse
import socket
import sys

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host',default='127.0.0.1')
    parser.add_argument('--port',type=int,default=6789)
    parser.add_argument('--file',help='Execute one SQL statement per non-comment line')
    args=parser.parse_args()
    with socket.create_connection((args.host,args.port),timeout=30) as conn:
        def execute(sql):
            conn.sendall(sql.encode('utf-8')+b'\0')
            data=bytearray()
            while b'\0' not in data:
                part=conn.recv(65536)
                if not part: raise RuntimeError('Server closed connection')
                data.extend(part)
            print(data.split(b'\0',1)[0].decode('utf-8',errors='replace').strip())
        if args.file:
            with open(args.file,encoding='utf-8-sig') as source:
                for line in source:
                    if line.strip() and not line.lstrip().startswith('--'):
                        print('miniob > '+line.strip())
                        execute(line.strip())
        else:
            while True:
                try: line=input('miniob > ').strip()
                except EOFError: break
                if line.lower() in ['quit','exit','quit;','exit;']: break
                if line: execute(line)

if __name__=='__main__': main()
