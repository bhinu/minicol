#!/usr/bin/env python3
"""Transport bounded source patches between disposable cloud jobs."""
import pathlib
import subprocess
import sys

MAX_PATCH_BYTES = 1_000_000
ROOT_FILES = {'README.md', 'Makefile'}
PREFIXES = ('src/', 'include/', 'tests/', 'bench/', 'docs/')


def git(*args):
    return subprocess.check_output(['git', *args])


def validate():
    paths = git('diff', '--cached', '--name-only', '-z').decode().split('\0')
    changed = [p for p in paths if p]
    for p in changed:
        parts = pathlib.PurePosixPath(p).parts
        if '..' in parts or p.startswith('/') or not (p in ROOT_FILES or p.startswith(PREFIXES)):
            raise ValueError(f'Cloud changes are not allowed in {p!r}')
    for entry in git('ls-files', '--stage', '-z').decode().split('\0'):
        if not entry:
            continue
        info, path = entry.split('\t', 1)
        if path in changed and info.split()[0] not in ('100644', '100755'):
            raise ValueError(f'Cloud changes cannot introduce symlinks or submodules: {path!r}')
    for required in ('Makefile', 'src/engine.cpp', 'include/minicol/engine.hpp', 'tests/test_engine.cpp'):
        if not pathlib.Path(required).is_file():
            raise ValueError(f'Required project file missing: {required}')
    subprocess.run(['git', 'diff', '--cached', '--check'], check=True)
    return changed


def capture(destination):
    subprocess.run(['git', 'add', '--all', '--', '.'], check=True)
    validate()
    patch = git('diff', '--cached', '--binary', '--full-index', '--no-ext-diff')
    if len(patch) > MAX_PATCH_BYTES:
        raise ValueError('Candidate patch exceeds 1 MB')
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(patch)
    print(f'Captured {len(patch)} patch bytes')


def apply(source):
    if source.stat().st_size > MAX_PATCH_BYTES:
        raise ValueError('Candidate patch exceeds 1 MB')
    if not source.stat().st_size:
        print('No changes proposed')
        return
    subprocess.run(['git', 'apply', '--check', '--index', str(source.resolve())], check=True)
    subprocess.run(['git', 'apply', '--index', str(source.resolve())], check=True)
    changed = validate()
    print(f'Applied {len(changed)} changed paths')


if __name__ == '__main__':
    try:
        if len(sys.argv) != 3 or sys.argv[1] not in ('capture', 'apply'):
            raise ValueError('Usage: cloud_patch.py capture|apply PATH')
        {'capture': capture, 'apply': apply}[sys.argv[1]](pathlib.Path(sys.argv[2]))
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        sys.exit(str(exc))
