"""Read-only plan/index and explicitly manifested artifact/test-log checks."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def check(root, plan='docs/session-plan.md', index='.cache/current-work.json', manifest=None):
    def file(value):
        return root / value
    pointer = json.loads(file(index).read_text(encoding='utf-8'))
    if not isinstance(pointer, dict):
        raise ValueError('checkpoint index object required')
    next_step = pointer.get('next_step')
    if not isinstance(next_step, str) or not next_step.strip() or next_step not in file(plan).read_text(encoding='utf-8'):
        raise ValueError('index next_step missing from plan')
    result = dict(next_step_consistent=True, artifacts_checked=0, test_logs_checked=0,
                  runtime_acceptance_verified=False,
                  limitations='Recorded-file consistency only; no freshness, live identity, semantic acceptance or completeness proof.')
    if manifest is None:
        return result
    evidence = json.loads(file(manifest).read_text(encoding='utf-8'))
    if not isinstance(evidence, dict):
        raise ValueError('checkpoint manifest object required')
    if evidence.get('version') != 1:
        raise ValueError('checkpoint manifest version 1 required')
    if evidence.get('next_step') != next_step:
        raise ValueError('manifest next_step differs from index')
    artifacts, tests = evidence.get('artifacts', []), evidence.get('test_runs', [])
    if not isinstance(artifacts, list) or not isinstance(tests, list) or len(artifacts)+len(tests) > 1000:
        raise ValueError('bounded artifact/test-run lists required')
    def verify(row):
        if not isinstance(row, dict) or not isinstance(row.get('path'), str) or not isinstance(row.get('sha256'), str) or not re.fullmatch(r'[0-9a-fA-F]{64}', row['sha256']):
            raise ValueError('explicit path and SHA-256 required')
        data = file(row['path']).read_bytes()
        if hashlib.sha256(data).hexdigest() != row['sha256'].lower():
            raise ValueError(f'artifact hash mismatch: {row["path"]}')
        return data
    for row in artifacts:
        verify(row)
        result['artifacts_checked'] += 1
    result['all_recorded_test_runs_passed'] = None
    for row in tests:
        data = verify(row)
        if row.get('kind') != 'ctest' or type(row.get('passed')) is not int or type(row.get('total')) is not int or not 0 <= row['passed'] <= row['total'] or row['total'] < 1:
            raise ValueError('ctest kind and explicit positive total/passed counts required')
        encoding = 'utf-16' if data.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig'
        summaries = re.findall(r'(\d+)% tests passed, (\d+) tests failed out of (\d+)', data.decode(encoding))
        if len(summaries) != 1:
            raise ValueError('one unambiguous CTest summary required per log')
        _, failed, total = map(int, summaries[0])
        if (total-failed, total) != (row['passed'], row['total']):
            raise ValueError('manifest test counts differ from log')
        result['test_logs_checked'] += 1
        result['all_recorded_test_runs_passed'] = (result['all_recorded_test_runs_passed'] is not False and failed == 0)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path.cwd())
    parser.add_argument('--plan', default='docs/session-plan.md')
    parser.add_argument('--index', default='.cache/current-work.json')
    parser.add_argument('--manifest', help='version 1 manifest; documented in checkpoint consistency recipes')
    args = parser.parse_args()
    try:
        print(json.dumps(check(args.root, args.plan, args.index, args.manifest), indent=2))
    except (ValueError, OSError, TypeError, AttributeError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
