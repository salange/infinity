#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Render the physical access routes exported by the canonical city manifest."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--manifest',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--route',help='Optional exact route ID; otherwise render every route')
    args=parser.parse_args()
    binary=args.binary.resolve(strict=True)
    manifest=json.loads(args.manifest.read_text())
    seed=manifest.get('seed')
    if not isinstance(seed,str) or not seed:
        parser.error('Manifest must include its scene seed; regenerate it with the current demo')
    routes=manifest['routes']
    if args.route:
        routes=[r for r in routes if r['id']==args.route]
        if not routes: parser.error('Unknown route ID')
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    if any(output.iterdir()): parser.error('--output must be empty')
    results=[]
    for route in routes:
        name=route['id']
        if not all(c.isalnum() or c in '_-' for c in name): raise ValueError('Invalid route ID')
        folder=output/name;folder.mkdir()
        path=folder/'camera-path.txt'
        path.write_text(''.join(' '.join(str(v) for v in p['position']+p['target'])+'\n' for p in route['waypoints']))
        def save_log(stdout,stderr):
            def decode(value):
                return value.decode('utf-8',errors='replace') if isinstance(value,bytes) else value or ''
            log=(decode(stdout)+decode(stderr)).replace(str(binary.parent),'<binary-dir>').replace(str(output),'<route-output>')
            (folder/'render.log').write_text(log)
            return log
        try:
            result=subprocess.run([str(binary),'--hidden','--shot','civic','--width','1280','--height','720',
                '--seed',seed,'--tex-size','512','--fov','61','--route',str(path),'--route-out',str(folder)],capture_output=True,text=True,timeout=14400)
        except subprocess.TimeoutExpired as error:
            save_log(error.stdout,error.stderr)
            raise RuntimeError(f'Route capture timed out; partial output retained in {name}/render.log') from error
        log=save_log(result.stdout,result.stderr)
        if result.returncode or '[wgpu] error' in log or 'FAILED' in log:
            raise RuntimeError(f'Route rendering failed: {name}')
        timing=json.loads((folder/'route-timings.json').read_text())
        sampling=json.loads((folder/'route-sampling.json').read_text())
        results.append(dict(id=name,waypoints=len(route['waypoints']),timings=timing,sampling=sampling,captures=[p.name for p in sorted(folder.glob('*.png'))]))
        print(f'{name}: {timing["frame_count"]} completed frames, {timing["max_ms"]:.1f} ms maximum',flush=True)
    (output/'routes.json').write_text(json.dumps(dict(schema_version=2,seed=seed,resolution=[1280,720],routes=results,
        interpretation='Initial waypoint and all connecting segments rendered at no more than 1 metre translation or 5 degrees rotation per update, with uninterrupted temporal history. Representative raw frames retained. Human inspection is required to assess obstruction and motion.'),indent=2)+'\n')


if __name__=='__main__': main()
