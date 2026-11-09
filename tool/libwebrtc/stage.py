#!/usr/bin/env python3
"""Stage the reviewed build recipe into a checkout of the libwebrtc fork.

Does not commit, push, dispatch Actions, or publish. Those are explicit steps.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent


def stage(checkout: Path) -> None:
    checkout = checkout.resolve()
    source = json.loads((HERE / 'source.json').read_text())
    base = subprocess.check_output(
        ['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip()
    pinned = source['wrapper_revision']
    # Permit restaging our existing fork branch, but never a different upstream
    # lineage. The resulting diff must still be reviewed before publishing.
    ancestor = subprocess.run(
        ['git', '-C', str(checkout), 'merge-base', '--is-ancestor', pinned, 'HEAD'],
        check=False, stderr=subprocess.PIPE)
    if ancestor.returncode != 0:
        raise SystemExit(f'Expected descendant of upstream {pinned}, got {base}')
    for required in ('BUILD.gn', 'include/rtc_peerconnection_factory.h', 'src'):
        if not (checkout / required).exists():
            raise SystemExit(f'Not a libwebrtc wrapper checkout: missing {required}')
    destination = checkout / 'discourse-build'
    destination.mkdir(exist_ok=True)
    for filename in ('source.json', 'build-windows.ps1'):
        shutil.copy2(HERE / filename, destination / filename)
    workflows = checkout / '.github/workflows'
    workflows.mkdir(parents=True, exist_ok=True)
    shutil.copy2(HERE / 'windows-build.yml', workflows / 'discourse-windows.yml')
    overlay = HERE / 'overlay'
    if overlay.exists():
        for file in sorted(overlay.rglob('*')):
            if file.is_file():
                target = checkout / file.relative_to(overlay)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(file, target)
    core = HERE / 'core-patches'
    if core.exists():
        shutil.copytree(core, destination / 'core-patches', dirs_exist_ok=True)
    print(f'Staged build recipe in {checkout}; review git diff before publishing.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('checkout', type=Path)
    stage(parser.parse_args().checkout)
