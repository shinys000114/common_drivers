# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
"""Private run isolation and an atomic, authoritative current-attempt record."""
from contextlib import contextmanager
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import tempfile
import uuid


def checked_paths(source_root, output_root):
    """Resolve symlinks and reject either containment direction without writes."""
    source_root = Path(source_root).resolve()
    output_root = Path(output_root).resolve()
    if (source_root == output_root or source_root in output_root.parents
            or output_root in source_root.parents):
        raise ValueError('output directory overlaps source root; choose a separate private directory')
    return source_root, output_root


def atomic_json(path, value):
    """The old record or the whole new record is visible, never partial JSON."""
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8',
                                         prefix='.status-', suffix='.tmp',
                                         dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(value, stream, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


class Attempt:
    def __init__(self, source_root, output_root, run_id, directory):
        self.output_root = output_root
        self.directory = directory
        self.run_id = run_id
        self.summary = None
        self.metadata = {
            'run_id': run_id,
            'started_at': datetime.now(timezone.utc).isoformat(),
            'source_root': str(source_root),
            'artifact_dir': str(directory),
            'validation': 'source/model only',
            'board_access': False,
            'production_changes': False,
        }

    def record(self, status, error=None):
        record = dict(self.metadata, status=status)
        if status == 'passed':
            if self.summary is None:
                raise RuntimeError('attempt has no completed result bundle')
            record.update(self.summary)
            record['source_manifest'] = str(self.directory / 'source-manifest.json')
        else:
            record['source_manifest'] = None
        if status != 'running':
            record['finished_at'] = datetime.now(timezone.utc).isoformat()
        if error is not None:
            record['error'] = f'{type(error).__name__}: {error}'
        return record

    def publish(self, status, error=None):
        record = self.record(status, error)
        # Invalidate old current PASS first; publish a completed new PASS last.
        if status != 'passed':
            atomic_json(self.output_root / 'summary.json', record)
        atomic_json(self.directory / 'summary.json', record)
        if status == 'passed':
            atomic_json(self.output_root / 'summary.json', record)


@contextmanager
def new_attempt(source_root, output_root):
    # Repeat the guard here so alternate callers cannot skip it.
    source_root, output_root = checked_paths(source_root, output_root)
    output_root.mkdir(parents=True, exist_ok=True)
    run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ-') + uuid.uuid4().hex[:8]
    directory = output_root / ('run-' + run_id)
    directory.mkdir()  # A fresh directory; never follows preexisting child links.
    attempt = Attempt(source_root, output_root, run_id, directory)
    previous_path = output_root / 'summary.json'
    previous = previous_path.read_bytes() if previous_path.exists() else None
    if previous is not None:
        attempt.metadata['historical_summary'] = str(directory / 'previous-summary.json')
    # The fresh run ID is visible before source reads, generated files or tools.
    attempt.publish('running')
    try:
        if previous is not None:
            (directory / 'previous-summary.json').write_bytes(previous)
        yield attempt
        attempt.publish('passed')
    except BaseException as error:
        attempt.publish('failed', error)
        raise
