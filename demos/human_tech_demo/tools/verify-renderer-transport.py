# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Paired offscreen-emitter proofs with disabled-transport negative controls."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zlib


def pixels(path):
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('invalid PNG')
    offset, payload = 8, bytearray()
    while offset < len(data):
        size = struct.unpack_from('>I', data, offset)[0]
        kind, chunk = data[offset+4:offset+8], data[offset+8:offset+8+size]
        if kind == b'IHDR':
            width, height, depth, color, _, _, interlace = struct.unpack('>IIBBBBB', chunk)
            if depth != 8 or color not in (2, 6) or interlace:
                raise ValueError('unsupported PNG')
            channels = 3 if color == 2 else 4
        elif kind == b'IDAT':
            payload.extend(chunk)
        offset += size+12
    raw, previous, rgb = zlib.decompress(payload), bytearray(width*channels), bytearray()
    for y in range(height):
        start = y*(width*channels+1)
        mode, row = raw[start], bytearray(raw[start+1:start+1+width*channels])
        for x in range(len(row)):
            a, b, c = (row[x-channels] if x >= channels else 0), previous[x], (previous[x-channels] if x >= channels else 0)
            p = a+b-c
            pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
            predictor = (0, a, b, (a+b)//2, a if pa <= pb and pa <= pc else b if pb <= pc else c)[mode]
            row[x] = (row[x]+predictor)&255
        for x in range(0,len(row),channels):
            rgb.extend(row[x:x+3])
        previous = row
    return width, height, rgb


def compare(a,b):
    aw,ah,ap=pixels(a);bw,bh,bp=pixels(b)
    if (aw,ah)!=(bw,bh): raise ValueError('mismatched dimensions')
    differences=[abs(x-y) for x,y in zip(ap,bp)]
    changed=sum(max(differences[i:i+3])>3 for i in range(0,len(differences),3))
    return {'mean_absolute_rgb_255':sum(differences)/len(differences),'maximum_channel_difference':max(differences),'changed_pixels_over_3':changed,'fraction_changed':changed/(aw*ah)}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();binary=args.binary.resolve(strict=True);out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    if any(out.iterdir()): parser.error('output must be empty')
    common=[str(binary),'--hidden','--sky','authored','--width','1280','--height','720','--tex-size','512','--frames','8']
    for kind in ('indirect','reflection'):
        for disabled in (False,True):
            for state in ('on','off'):
                label=f'{kind}-'+('disabled-' if disabled else '')+state
                flags=['--asset',f'{kind}-proof-{state}','--capture',str(out/(label+'.png'))]
                if kind=='indirect': flags+=['--debug','7','--no-reflections']
                else: flags+=['--no-indirect']
                if disabled: flags+=['--no-indirect','--no-reflections']
                print('Capturing',label,flush=True)
                result=subprocess.run(common+flags,capture_output=True,text=True,timeout=600)
                log=(result.stdout+result.stderr).replace(str(out),'<output-dir>').replace(str(binary.parent),'<binary-dir>');(out/(label+'.log')).write_text(log)
                if result.returncode or 'Validation Error' in log: raise RuntimeError(f'{label} failed')
    report={'resolution':[1280,720],'proofs':{},'source_sha256':{}}
    demo=Path(__file__).resolve().parents[1]
    for relative in ('src/renderer_proof.hpp','src/voxel_transport.hpp','shaders/main.wgsl','shaders/ssao.wgsl'):
        report['source_sha256'][relative]=hashlib.sha256((demo/relative).read_bytes()).hexdigest()
    for kind in ('indirect','reflection'):
        active=compare(out/(kind+'-on.png'),out/(kind+'-off.png'))
        negative=compare(out/(kind+'-disabled-on.png'),out/(kind+'-disabled-off.png'))
        passed=active['changed_pixels_over_3']>100 and active['maximum_channel_difference']>20 and negative['maximum_channel_difference']==0
        report['proofs'][kind]={'active':active,'disabled_negative_control':negative,'passed':passed}
    report['passed']=all(p['passed'] for p in report['proofs'].values())
    (out/'offscreen-proof.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2),flush=True)
    if not report['passed']: raise SystemExit(1)

if __name__=='__main__': main()
