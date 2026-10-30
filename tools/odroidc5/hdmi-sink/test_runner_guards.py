#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
"""Bounded path/freshness tests. No policy matrix, hardware or source edits."""
import sys
sys.dont_write_bytecode = True
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
from run_state import new_attempt

HERE = Path(__file__).resolve().parent
RUNNER = HERE / 'run.py'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-root', type=Path, required=True)
SOURCE = parser.parse_args().source_root.resolve()
checks = []


def snapshot(path):
    result = {}
    for p in sorted(path.rglob('*')):
        if p.is_symlink():
            value = ('link', os.readlink(p))
        elif p.is_file():
            value = ('file', hashlib.sha256(p.read_bytes()).hexdigest())
        else:
            value = ('directory',)
        result[str(p.relative_to(path))] = value
    return result


def cli(root, out, env=None):
    return subprocess.run([sys.executable, str(RUNNER), '--source-root', str(root),
                           '--out', str(out)], text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=20, env=env)


def legacy(out):
    out.mkdir(parents=True)
    previous = {'run_id': 'historical-test-pass', 'status': 'passed',
                'validation': 'test fixture, not actual matrix evidence', 'policy_cases': 123}
    data = (json.dumps(previous) + '\n').encode()
    (out / 'summary.json').write_bytes(data)
    return data


def verify_failed(out, previous):
    current = json.loads((out / 'summary.json').read_text())
    assert current['status'] == 'failed' and current['run_id'] != 'historical-test-pass'
    assert 'policy_cases' not in current and current['source_manifest'] is None
    assert Path(current['historical_summary']).read_bytes() == previous
    bundle = Path(current['artifact_dir'])
    assert json.loads((bundle / 'summary.json').read_text()) == current
    assert not (bundle / 'source-manifest.json').exists()
    return current, bundle


# Every rejected argument combination must leave its entire private tree intact.
for case in ['equal', 'new-child', 'existing-child', 'ancestor', 'dotdot',
             'output-symlink', 'source-symlink']:
    with tempfile.TemporaryDirectory(prefix='c5-runner-path-') as temporary:
        base = Path(temporary)
        source = base / 'scratch/source'
        source.mkdir(parents=True)
        (source / 'sentinel').write_text('unchanged source\n')
        root = source
        if case == 'equal': out = source
        elif case == 'new-child': out = source / 'new/output'
        elif case == 'existing-child':
            out = source / 'existing'; out.mkdir(); (out / 'sentinel').write_text('old data\n')
        elif case == 'ancestor': out = base / 'scratch'
        elif case == 'dotdot': out = source / '../source/subdirectory'
        elif case == 'output-symlink':
            alias = base / 'alias'; alias.symlink_to(source, target_is_directory=True); out = alias / 'new'
        else:
            root = base / 'source-alias'; root.symlink_to(source, target_is_directory=True)
            out = source / 'new'
        before = snapshot(base)
        result = cli(root, out)
        assert result.returncode == 2 and 'overlaps source root' in result.stderr, result.stderr
        assert snapshot(base) == before, case
        checks.append('reject-without-writes:' + case)

# Allowed sibling, failure before the first generated source/header write.
with tempfile.TemporaryDirectory(prefix='c5-runner-early-') as temporary:
    base = Path(temporary); source = base / 'source'; source.mkdir()
    (source / 'sentinel').write_text('unchanged\n')
    before = snapshot(source)
    out = base / 'source-results'; old = legacy(out)
    result = cli(source, out)  # Intentionally missing the first read-only source input.
    assert result.returncode != 0 and 'FileNotFoundError' in result.stderr
    current, bundle = verify_failed(out, old)
    assert not (bundle / 'include').exists() and not (bundle / 'policy.c').exists()
    assert snapshot(source) == before
    checks.append('early-read-failure-invalidates-pass')

# Observe the running marker before any body work, then fail after preparing a result.
with tempfile.TemporaryDirectory(prefix='c5-runner-state-') as temporary:
    base = Path(temporary); source = base / 'source'; source.mkdir()
    out = base / 'results'; old = legacy(out)
    try:
        with new_attempt(source, out) as attempt:
            running = json.loads((out / 'summary.json').read_text())
            assert running['status'] == 'running' and running['run_id'] == attempt.run_id
            assert 'policy_cases' not in running and running['source_manifest'] is None
            attempt.summary = {'policy_cases': 39}
            raise RuntimeError('injected metadata failure after tests')
    except RuntimeError as error:
        assert str(error) == 'injected metadata failure after tests'
    else:
        raise AssertionError('failure was swallowed')
    verify_failed(out, old)
    checks.append('running-marker-before-work-and-late-failure')

# Exercise the actual runner with a compiler that fails immediately; no C tests run.
with tempfile.TemporaryDirectory(prefix='c5-runner-compiler-') as temporary:
    base = Path(temporary); out = base / 'results'; old = legacy(out)
    bindir = base / 'bin'; bindir.mkdir()
    compiler = bindir / 'cc'
    compiler.write_text('#!/bin/sh\nexit 73\n'); compiler.chmod(0o700)
    environment = dict(os.environ, CC='cc', PATH=str(bindir) + os.pathsep + os.environ['PATH'])
    result = cli(SOURCE, out, environment)
    assert result.returncode != 0 and '73' in result.stderr
    _, bundle = verify_failed(out, old)
    assert (bundle / 'policy.c').exists() and not (bundle / 'policy').exists()
    assert not (bundle / 'edid').exists()
    checks.append('actual-runner-compiler-failure-invalidates-pass')

print(json.dumps({'status': 'passed', 'checks': checks, 'count': len(checks),
                  'full_matrix_runs': 0, 'board_access': False}, indent=2))
