#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Build the editable authored kit, checked glTF, and runtime resources locally."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--blender',default='blender')
parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'assets'/'generated')
parser.add_argument('--compile-only',action='store_true')
args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
here=Path(__file__).resolve().parent
if not args.compile_only:
    executable=shutil.which(args.blender)
    if not executable: raise SystemExit('Blender 5.2 is required to reproduce the authored source kit.')
    env=dict(os.environ,ALSOFT_DRIVERS='null',XDG_CACHE_HOME=str(args.output/'.cache'))
    log=args.output/'authoring.log'
    with log.open('w') as stream:
        result=subprocess.run([executable,'--background','-t','1','-noaudio','--factory-startup','--python-exit-code','1','--python',str(here/'author-kit-blender.py'),'--',str(args.output)],stdout=stream,stderr=subprocess.STDOUT,env=env)
    if result.returncode:
        print(log.read_text()[-6000:],file=sys.stderr)
        raise SystemExit(f'Blender authoring failed ({result.returncode}); full log: {log}')
    print('Blender source and glTF exported; authoring log:',log)
subprocess.run([sys.executable,str(here/'canonicalize-gltf-kit.py'),str(args.output/'human_tech_kit.authoring.glb'),str(args.output/'human_tech_kit.glb')],check=True)
subprocess.run([sys.executable,str(here/'compile-gltf-kit.py'),str(args.output/'human_tech_kit.glb'),str(args.output/'human_tech_kit.htkit'),'--manifest',str(args.output/'manifest.json')],check=True)
manifest_path=args.output/'manifest.json';manifest=json.loads(manifest_path.read_text())
manifest['recipe']={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (here/'author-kit-blender.py',here/'canonicalize-gltf-kit.py',here/'compile-gltf-kit.py')}
manifest['artifacts']={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in args.output.iterdir() if p.suffix in ('.blend','.glb','.htkit')}
manifest_path.write_text(json.dumps(manifest,indent=2)+'\n')
print('Ready:',args.output/'human_tech_kit.htkit')
subprocess.run([sys.executable,str(here/'build-arrival-towers.py'),'--blender',args.blender,'--output',str(args.output)]+
               (['--compile-only'] if args.compile_only else []),check=True)
