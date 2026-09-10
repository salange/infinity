#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Build the editable six-tower Blender library and checked native resource kit."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--blender',default='blender');p.add_argument('--compile-only',action='store_true')
p.add_argument('--inspect',action='store_true')
p.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'assets'/'generated')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True);a.output=a.output.resolve()
here=Path(__file__).resolve().parent
if not a.compile_only:
    executable=shutil.which(a.blender)
    if not executable:raise SystemExit('Blender is required to build the First Arrival tower assets.')
    env=dict(os.environ,ALSOFT_DRIVERS='null',XDG_CACHE_HOME=str(a.output/'.cache'))
    command=[executable,'--background','-t','1','-noaudio','--factory-startup','--python-exit-code','1',
             '--python',str(here/'author-arrival-towers-blender.py'),'--',str(a.output)]
    if a.inspect:command.append('--inspect')
    log=a.output/'arrival-authoring.log'
    with log.open('w') as stream:
        result=subprocess.run(command,stdout=stream,stderr=subprocess.STDOUT,env=env)
    if result.returncode:
        print(log.read_text()[-6000:],file=sys.stderr);raise SystemExit(f'Blender authoring failed ({result.returncode})')
    print('Editable Blender source and glTF exported:',log,flush=True)
subprocess.run([sys.executable,str(here/'canonicalize-gltf-kit.py'),str(a.output/'arrival_towers.authoring.glb'),str(a.output/'arrival_towers.glb')],check=True)
subprocess.run([sys.executable,str(here/'compile-gltf-kit.py'),str(a.output/'arrival_towers.glb'),str(a.output/'arrival_towers.htkit'),
                '--manifest',str(a.output/'arrival-towers-manifest.json')],check=True)
manifest_path=a.output/'arrival-towers-manifest.json';manifest=json.loads(manifest_path.read_text())
manifest['placements']=json.loads((a.output/'arrival-towers-spec.json').read_text())
recipes=[here/'arrival_tower_geometry.py',here/'arrival_hero_tower.py',here/'arrival_left_towers.py',here/'arrival_right_towers.py',
         here/'author-arrival-towers-blender.py',here/'canonicalize-gltf-kit.py',here/'compile-gltf-kit.py']
manifest['recipe']={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in recipes}
manifest['artifacts']={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(a.output.glob('arrival*')) if f.suffix in ('.blend','.glb','.htkit')}
manifest_path.write_text(json.dumps(manifest,indent=2)+'\n')
print('Ready:',a.output/'arrival_towers.htkit')
