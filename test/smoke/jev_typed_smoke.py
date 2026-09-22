#!/usr/bin/env python3
"""Local HTTP coverage for typed Jev decisions, row packing and downstream SQL."""

import json
import os
import re
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


SPEC = """{
    department: MAP {'billing': 'Payments', 'technical': 'Bugs'},
    severity: ['Routine', 'Degraded', 'Blocked'],
    urgent: MAP {'true': 'Urgent', 'false': 'Can wait'}
}"""


def run(duckdb_path):
    requests = []
    mode = 'normal'

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            raw = self.rfile.read(int(self.headers['Content-Length']))
            body = json.loads(raw)
            requests.append((body, len(raw), self.headers.get('Authorization')))
            partial_failure = mode == 'partial_error' and any(
                question['instructions']['record'].startswith('32:') for question in body['questions'].values()
            )
            if mode == 'http_error' or partial_failure or (mode == 'retry' and len(requests) == 1):
                self.send_response(503)
                self.end_headers()
                self.wfile.write(b'{"error":"temporary failure"}')
                return
            answers = {}
            for key, question in reversed(list(body['questions'].items())):
                record = question['instructions']['record']
                prefix = record.split(':')[0]
                index = int(prefix) if prefix.isdigit() else int('blocked' in record.lower())
                kind = question['type']
                if kind == 'choice':
                    labels = list(question['criteria'])
                    label = labels[index % len(labels)]
                    answers[key] = {
                        'type': kind,
                        'choice': label,
                        'confidence': 0.9,
                        'probabilities': {option: float(option == label) for option in labels},
                    }
                elif kind == 'score':
                    answers[key] = {
                        'type': kind,
                        'score': index % len(question['criteria']),
                        'confidence': 0.8,
                        'probabilities': {'0': 1.0},
                    }
                else:
                    answers[key] = {'type': kind, 'noul': 0.75 if index % 2 else 0.25}
            first_key = next(iter(body['questions']))
            answer_mode = mode
            if mode.endswith('_once'):
                answer_mode = mode.removesuffix('_once') if len(requests) == 1 else 'normal'
            if answer_mode == 'missing':
                answers.pop(first_key)
            elif answer_mode == 'wrong_type':
                answers[first_key] = {'type': 'noul', 'noul': 0.5}
            elif answer_mode == 'bad_choice':
                answers[first_key]['choice'] = 'undeclared'
            elif answer_mode == 'bad_score':
                next(answer for answer in answers.values() if answer['type'] == 'score')['score'] = 8.0
            elif answer_mode == 'bad_confidence':
                answers[first_key]['confidence'] = 1.2
            elif answer_mode == 'no_confidence':
                for answer in answers.values():
                    answer.pop('confidence', None)
            data = json.dumps(
                {
                    'model': 'jev-1.13.0',
                    'answers': answers,
                    'usage': {'input_tokens': 100, 'output_tokens': 20},
                }
            ).encode()
            if answer_mode == 'duplicate':
                text = data.decode()
                field = json.dumps(first_key) + ': ' + json.dumps(answers[first_key])
                data = text.replace('"answers": {', '"answers": {' + field + ', ', 1).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = {k: v for k, v in os.environ.items() if not k.startswith(('DUCKDB_AI_', 'TYPESAFE_'))}
    env.update(TYPESAFE_API_KEY='jev-mock', TYPESAFE_BASE_URL=f'http://127.0.0.1:{server.server_port}/v1')

    def query(sql, error=None, credentials=True):
        query_env = env.copy()
        if not credentials:
            query_env.pop('TYPESAFE_API_KEY')
        result = subprocess.run(
            [str(duckdb_path), '-unsigned', '-batch', '-bail', '-json'],
            input='LOAD ai; SET threads=1; ' + sql,
            env=query_env,
            text=True,
            capture_output=True,
            timeout=60,
        )
        if error:
            assert result.returncode != 0 and error in result.stderr, result.stdout + result.stderr
            return
        assert result.returncode == 0, result.stdout + result.stderr
        output = result.stdout.strip()
        datasets = []
        while output:
            rows, end = json.JSONDecoder().raw_decode(output)
            datasets.append(rows)
            output = output[end:].lstrip()
        return datasets[-1] if datasets else []

    def decisions(sql, **kwargs):
        rows = query("CREATE TEMP TABLE got AS " + sql + "; SELECT i, a.*, a IS NULL AS is_null FROM got;", **kwargs)
        if rows is None:
            return None
        for row in rows:
            if row['is_null']:
                assert all(value is None for key, value in row.items() if key not in ('i', 'is_null')), row
        return [
            {
                'i': row['i'],
                'a': (
                    None
                    if row['is_null']
                    else {key: value for key, value in row.items() if key not in ('i', 'is_null')}
                ),
            }
            for row in rows
        ]

    def select(count, options='', text="i::VARCHAR || ': quoted ''é'' ' || chr(10)"):
        return f"SELECT i, ai_jev({text}, {SPEC}, retry_count := 0{options}) AS a FROM range({count}) t(i)"

    try:
        for count in (0, 1, 31, 32, 33, 65, 2049):
            requests.clear()
            rows = decisions(select(count))
            assert len(rows) == count, rows
            assert sorted(len(body['questions']) for body, _, _ in requests) == sorted(
                min(32, count - offset) * 3 for offset in range(0, count, 32)
            ), requests
            for row in rows:
                answer = row['a']
                assert answer == {
                    'department': 'technical' if row['i'] % 2 else 'billing',
                    'department_confidence': 0.9,
                    'severity': row['i'] % 3,
                    'severity_confidence': 0.8,
                    'urgent': 0.75 if row['i'] % 2 else 0.25,
                }, row
            assert all(body['state'] == '' and key == 'Bearer jev-mock' for body, _, key in requests)
        requests.clear()
        decisions(select(65, ', batch_size := 1'))
        assert len(requests) == 65
        requests.clear()
        rows = decisions(select(65, text="CASE WHEN i % 2 = 0 THEN NULL ELSE i::VARCHAR || ':text' END"))
        assert len(requests) == 1 and len(requests[0][0]['questions']) == 96
        assert all((r['a'] is None) == (r['i'] % 2 == 0) for r in rows)
        requests.clear()
        assert decisions(f"SELECT 0 AS i, ai_jev(NULL, {SPEC}) AS a", credentials=False) == [{'i': 0, 'a': None}]
        assert not requests
        decisions(select(4, ', max_request_bytes := 2048'))
        assert len(requests) > 1 and all(size <= 2048 for _, size, _ in requests)
        requests.clear()
        query(
            select(1, ', max_request_bytes := 1024', text="'0:' || repeat('x', 2000)"),
            error='exceeds max_request_bytes',
        )
        assert not requests
        rows = decisions(
            select(
                3,
                ", max_request_bytes := 2048, on_error := 'null'",
                text="i::VARCHAR || ':' || CASE WHEN i=1 THEN repeat('x', 3000) ELSE 'ok' END",
            )
        )
        assert [r['a'] is None for r in rows] == [False, True, False]
        assert sum(len(body['questions']) for body, _, _ in requests) == 6
        for mode, error in (
            ('missing', 'missing or mismatched'),
            ('duplicate', 'duplicate answer keys'),
            ('wrong_type', 'missing or mismatched'),
            ('bad_choice', 'outside the declared choices'),
            ('bad_score', 'outside the declared scale'),
            ('bad_confidence', 'valid model and answers'),
            ('http_error', '503'),
        ):
            decisions(select(2), error=error)
            assert all(r['a'] is None for r in decisions(select(2, ", on_error := 'null'")))
        # A 2xx response can still violate the typed contract. It must be evicted
        # and recorded as an error before an identical call can use the cache.
        for mode in ('missing_once', 'bad_choice_once', 'bad_score_once', 'duplicate_once'):
            requests.clear()
            call = f"ai_jev('0:cache', {SPEC}, cache := true, on_error := 'null', retry_count := 0)"
            rows = query(
                f"""
CREATE TEMP TABLE first_result AS SELECT {call} AS a;
CREATE TEMP TABLE second_result AS SELECT {call} AS a;
CREATE TEMP TABLE third_result AS SELECT {call} AS a;
SELECT (SELECT a IS NULL FROM first_result) AS rejected,
       (SELECT a.department FROM second_result) AS recovered,
       (SELECT a.department FROM third_result) AS cached,
       count(*) FILTER (WHERE status='error') AS errors,
       count(*) FILTER (WHERE status='ok') AS successes,
       count(*) FILTER (WHERE cache_hit) AS cache_hits
FROM ai_usage() WHERE function_name='ai_jev';
"""
            )
            assert rows == [
                {
                    'rejected': True,
                    'recovered': 'billing',
                    'cached': 'billing',
                    'errors': 1,
                    'successes': 2,
                    'cache_hits': 1,
                }
            ], rows
            assert len(requests) == 2, requests
        mode = 'partial_error'
        requests.clear()
        rows = decisions(select(65, ", on_error := 'null'"))
        assert len(requests) == 3
        assert [r['i'] for r in rows if r['a'] is None] == list(range(32, 64))
        mode = 'no_confidence'
        rows = decisions(select(1))
        assert rows[0]['a']['department_confidence'] is None and rows[0]['a']['severity_confidence'] is None
        mode = 'retry'
        requests.clear()
        rows = decisions(f"SELECT 0 AS i, ai_jev('0:retry', {SPEC}, retry_count := 1, retry_backoff_ms := 0) AS a")
        assert rows[0]['a']['department'] == 'billing' and len(requests) == 2
        mode = 'normal'
        requests.clear()
        with tempfile.TemporaryDirectory() as directory:
            target = str(Path(directory) / 'decisions.parquet').replace("'", "''")
            rows = query(
                f"""
CREATE TEMP TABLE decisions AS {select(65)};
COPY (SELECT i, a.* FROM decisions) TO '{target}' (FORMAT PARQUET);
SELECT (SELECT count(*) FROM read_parquet('{target}')) AS exported_rows,
       (SELECT sum(severity) FROM read_parquet('{target}')) AS severity_sum,
       count(*) AS calls, sum(prompt_tokens)::BIGINT AS tokens
FROM ai_usage() WHERE function_name='ai_jev' AND event='ai_completion';
"""
            )
            assert rows == [{'exported_rows': 65, 'severity_sum': 64.0, 'calls': 3, 'tokens': 300}], rows
            assert len(requests) == 3
        # Repeated projections of saved values never rerun inference.
        requests.clear()
        rows = query(f"CREATE TEMP TABLE d AS {select(2)}; SELECT i, a.*, a.urgent > 0.5 AS urgent FROM d;")
        assert len(rows) == 2 and len(requests) == 1
        # Run the published cookbook SQL verbatim, including a real Parquet export.
        root = Path(__file__).resolve().parents[2]
        cookbook = (root / 'docs/cookbooks/jev-decisions.md').read_text()
        sql = '\n'.join(re.findall(r'```sql\n(.*?)```', cookbook, re.S))
        requests.clear()
        with tempfile.TemporaryDirectory() as directory:
            completed = subprocess.run(
                [str(duckdb_path), '-unsigned', '-batch', '-bail'],
                input=sql,
                env=env,
                cwd=directory,
                text=True,
                capture_output=True,
                timeout=60,
            )
            assert completed.returncode == 0, completed.stdout + completed.stderr
            assert len(requests) == 1 and len(requests[0][0]['questions']) == 6
            exported = str(Path(directory) / 'ticket_decisions.parquet').replace("'", "''")
            assert query(f"SELECT count(*) AS n FROM read_parquet('{exported}');") == [{'n': 3}]
        # The shorter source-checkout recipe also runs without the JSON extension.
        requests.clear()
        recipe = (root / 'examples/jev_batch_rows.sql').read_text()
        rows = query(
            "CREATE TEMP TABLE jev_batch_input AS SELECT i source_id, i::VARCHAR || ':ticket' body FROM range(65) t(i);"
            + recipe
        )
        assert len(rows) == 65 and len(requests) == 3
        # Credentials and pinned model use the existing secret path.
        requests.clear()
        rows = query(
            f"""
CREATE SECRET local_jev (TYPE duckdb_ai, AI_PROVIDER 'typesafe', API_KEY 'secret-mock', MODEL 'jev-preview');
CREATE TEMP TABLE secret_result AS SELECT ai_jev('0:secret', {SPEC}, secret := 'local_jev') AS a;
SELECT a.department AS department FROM secret_result;
""",
            credentials=False,
        )
        assert rows[0]['department'] == 'billing'
        assert requests[0][0]['model'] == 'jev-preview' and requests[0][2] == 'Bearer secret-mock'
        print(
            'typed Jev smoke passed: 65 rows in 3 requests, typed mixed answers, byte limits, failures, retries, usage, Parquet'
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == '__main__':
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--duckdb', type=Path, default=Path(__file__).resolve().parents[2] / 'build/release/duckdb')
    run(parser.parse_args().duckdb.resolve())
