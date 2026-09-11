# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Build the illustrated tower library from exact native diagnostic exports."""
import argparse
import hashlib
import html
import json
import os
from pathlib import Path
import subprocess
import sys

HERE=Path(__file__).resolve().parent
GAME=HERE.parents[2]
ASSETS=[]
def entry(key,name,group,source,description,use,kind='native',note=''):
    ASSETS.append(dict(key=key,name=name,group=group,source=source,description=description,use=use,kind=kind,note=note))

entry('arrival_hex','Ivory cellular tower','arrival','demos/human_tech_demo/tools/arrival_left_towers.py',
      'Deep chamfered ivory exoskeleton with alternating elongated and short hexagonal openings over bronze glazing.',
      'Leftmost of the six towers opposite the station. Shaft resource; its base is listed separately.','kit')
entry('arrival_blade','Graphite blade','arrival','demos/human_tech_demo/tools/arrival_left_towers.py',
      'Slim rounded-oblong shaft with broad charcoal panels, recessed vertical windows and bronze jambs.',
      'Second tower from the left. Narrow skyline punctuation.','kit')
entry('arrival_ribbon','Pale ribbon tower','arrival','demos/human_tech_demo/tools/arrival_left_towers.py',
      '38 curved white floor ribbons follow a waisted profile and modest upper flare.',
      'Immediately left of the graphite hero. Shaft resource.','kit')
entry('arrival_hero','Graphite ellipse','arrival','demos/human_tech_demo/tools/arrival_hero_tower.py',
      '45 occupied floors in a genuinely elongated ellipse, with tapered shoulders, an inclined rounded crown and fine bronze framing.',
      'Large black tower opposite the station. Its rounded socket appears in the base-building section.','kit',
      'Private smoked-pane material; the five neighboring tower recipes are independent.')
entry('arrival_bronze','Bronze oval','arrival','demos/human_tech_demo/tools/arrival_right_towers.py',
      'Shorter 21-floor oval with fine pale bands, bronze framing and an inset rooftop pavilion.',
      'Right of the hero, in front of the diamond tower.','kit')
entry('arrival_diamond','Ivory diamond tower','arrival','demos/human_tech_demo/tools/arrival_right_towers.py',
      '37-floor rounded shaft with deep curved white diagonal members, separate bronze framing and a recessed roof pavilion.',
      'Behind the bronze oval on the right of the six-tower group.','kit')
entry('foreground-lattice','Foreground lattice tower','landmarks','demos/human_tech_demo/src/scene.cpp · foreground()',
      'The complete 136-floor foreground tower, sculpted ceramic exoskeleton, occupied loggias and attached garden structure.',
      'The accepted landmark at the left edge of First Arrival.',
      note='Includes actual attached assemblies and kit instances; this is not the small facade inspection patch.')
entry('canal-round','Round canal tower','landmarks','demos/human_tech_demo/src/canal_tower.cpp',
      'Graduated bronze round shaft with shared hexagonal members, a dark neck and a layered open crown surrounding a raised pavilion.',
      'The small round tower at the bridge-side lot. Its integrated crown is also shown separately below.',
      note='The tower core extends below the visible shaft to its occupied lot; the complete generator is counted.')
entry('garden-companion','Garden companion tower','landmarks','demos/human_tech_demo/src/scene.cpp · garden_companion()',
      '80-floor round companion with pale floor bands, occupied blue glazing, roof garden and real bridge-entry openings.',
      'Companion to the foreground tower’s upper garden route.')
entry('oval-landmark','Broad oval lattice landmark','landmarks','demos/human_tech_demo/src/scene.cpp · oval_landmark()',
      'Broad elliptical curtain wall with a full ceramic crossing network and an occupied sky court.',
      'Retained scene landmark outside the six new Arrival shafts.')
entry('civic-needle','Civic needle','landmarks','demos/human_tech_demo/src/scene.cpp · civic_tower_variant()',
      '110-floor tapered needle with bronze members and a strong tip, on its occupied supporting base.',
      'Tall civic skyline marker. Exact composed variant of the shared procedural tower family.')
entry('civic-ribbon','Civic ribbon','landmarks','demos/human_tech_demo/src/scene.cpp · civic_tower_variant()',
      '43-floor broad ribbon variant with a separate four-storey occupied base.',
      'Retained civic-area tower; distinct from the new Arrival ribbon model.')
entry('civic-lens','Civic bronze lens','landmarks','demos/human_tech_demo/src/scene.cpp · civic_tower_variant()',
      '44-floor bronze lens variant with a rounded occupied base.',
      'Retained east civic tower; distinct from the new Arrival bronze oval.')
for key,label in [('hex','Cellular'),('blade','Blade'),('ribbon','Ribbon'),('hero','Graphite hero'),('bronze','Bronze oval'),('diamond','Diamond')]:
    entry('arrival-base-'+key,label+' base building','bases',
          'demos/human_tech_demo/src/arrival_tower_lots.cpp'+(' + arrival_hero_tower.py' if key=='hero' else ''),
          'Complete occupied supporting building, authored for the street-aligned common block.',
          'Supports the '+label.lower()+' shaft. Individual base geometry is shown without its tower or the shared street court.',
          note='Final iteration 31 base export; surrounding block paving is accounted for in the scene report.')
families=[
    ('diagrid','Diagrid','Diagonal structural lattice around a superellipse.'),
    ('lens','Lens','Elliptical/lens plan with a slender silhouette.'),
    ('sail','Sail','Sail profile with directional vertical facade structure.'),
    ('finweave','Fin weave','Closely spaced woven fins around a curved shaft.'),
    ('xframe','X-frame','Cross-braced facade on a broad, lower tower.'),
    ('hex','Hex lattice','Procedural hexagonal facade network.'),
    ('curtain','Curtain wall','Plain occupied curtain wall with adjustable plans and proportions.'),
    ('louvre','Louvre','Round shaft with layered facade louvres.'),
    ('ribbon','Ribbon','Continuous procedural floor ribbons.'),
    ('random','Seeded random variant','A reproducible example selected from the shared family space.')]
for key,label,description in families:
    entry('tower:'+key,label+' generator','generators','game/city/src/towers.cpp + demos/human_tech_demo/src/catalog.cpp',
          description,'Reusable family, not a stored unique building. Example: seed 83, default catalog parameters, detail 2.',
          note='The image represents one deterministic parameter set. Dimensions and polygon counts change with parameters.')
entry('group','Tower group generator','generators','game/city/src/towers.cpp · build_tower_group()',
      'A complete composed group produced by the reusable tower-group generator.',
      'Deterministic seed-83 example; the whole generated group is framed and counted.',
      note='A generator example, not an additional hand-authored landmark.')
entry('sculpted-lattice-module','Sculpted facade assembly','parts','demos/human_tech_demo/src/sculpted_diagrid.cpp',
      'A complete bounded section of the production deep ceramic grid, with matching recessed glazing and sample floors.',
      'Reusable facade construction and inspection assembly for the foreground lattice tower.',
      note='Explicit bounded sample of the larger generator, not another whole tower.')
entry('canal-crown','Round tower crown assembly','parts','demos/human_tech_demo/src/canal_tower.cpp',
      'The existing open ring stack, annular deck, fine uprights, raised pavilion and folded exhaust cowl.',
      'An exact triangle extraction of the integrated canal-tower crown, for inspection.',
      note='Integral generator part; no independent runtime model file. Lower cutoff includes the full crown deck.')
entry('ceramic_lattice_node','Library ceramic lattice node','parts','demos/human_tech_demo/tools/author-kit-blender.py',
      'The earlier compact ceramic crossing component retained in the general authored resource library.',
      'Reusable library component; the foreground tower uses the later sculpted native grid instead.','base-kit',
      'Retained component, not the current foreground facade itself.')

GROUPS=[('arrival','Six authored Arrival towers'),('landmarks','Other current landmark towers'),
        ('bases','Six occupied base buildings'),('generators','Reusable procedural families'),('parts','Facade and crown parts')]

def short_bytes(value):
    for unit in ('B','KiB','MiB','GiB'):
        if value<1024 or unit=='GiB':return f'{value:,.0f} {unit}' if unit=='B' else f'{value:,.2f} {unit}'
        value/=1024

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--game',type=Path,default=GAME)
    p.add_argument('--render',action='store_true');p.add_argument('--groups',default='arrival,landmarks,bases,generators,parts')
    p.add_argument('--force',action='store_true');p.add_argument('--blender',default='blender')
    args=p.parse_args();game=args.game.resolve();output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    working=game/'build/square-block-31/inventory';working.mkdir(parents=True,exist_ok=True)
    chosen=set(args.groups.split(','));base_kit=game/'demos/human_tech_demo/assets/generated/human_tech_kit.htkit'
    arrival_kit=base_kit.with_name('arrival_towers.htkit');exporter=game/'build/demos/human_tech_demo/human_tech_tower_inventory'
    for asset in ASSETS:
        key=asset['key'];folder=key.replace(':','-');dest=output/'assets'/folder
        if not args.render or asset['group'] not in chosen:continue
        work=working/folder;work.mkdir(parents=True,exist_ok=True)
        if asset['kind'] in ('kit','base-kit'):
            source=arrival_kit if asset['kind']=='kit' else base_kit
            selection=['--resource',key]
        else:
            with (work/'export.log').open('w') as log:
                subprocess.run([str(exporter),key,str(base_kit),str(work)],stdout=log,stderr=subprocess.STDOUT,check=True)
            source=work/'mesh.json';selection=[]
        if not args.force and (dest/'full.png').exists() and (dest/'metadata.json').exists():
            previous=json.loads((dest/'metadata.json').read_text())
            binary=source if source.suffix=='.htkit' else work/'mesh.bin'
            if (previous.get('source_sha256')==hashlib.sha256(source.read_bytes()).hexdigest()
                and previous.get('geometry_binary_sha256')==hashlib.sha256(binary.read_bytes()).hexdigest()
                and previous.get('image_sha256')==hashlib.sha256((dest/'full.png').read_bytes()).hexdigest()
                and previous.get('render_recipe_sha256')==hashlib.sha256((HERE/'render-tower-inventory.py').read_bytes()).hexdigest()):continue
        with (work/'render.log').open('w') as log:
            subprocess.run([args.blender,'--background','--factory-startup','-t','2','-noaudio','--python',
                            str(HERE/'render-tower-inventory.py'),'--','--source',str(source),
                            *selection,'--output',str(dest),'--elevation','20' if asset['group'] in ('bases','parts') else '12'],
                           check=True,stdout=log,stderr=subprocess.STDOUT,env=dict(os.environ,ALSOFT_DRIVERS='null'))
        print('Rendered:',key,flush=True)
    rows=[];missing=[]
    for asset in ASSETS:
        folder=asset['key'].replace(':','-');path=output/'assets'/folder/'metadata.json'
        record=dict(asset,folder=folder)
        if path.exists() and (path.parent/'full.png').exists():record['measured']=json.loads(path.read_text())
        else:missing.append(asset['key'])
        rows.append(record)
    surface_manifest=json.loads((game/'demos/human_tech_demo/assets/surface-manifest.json').read_text())
    surface_sources={s['name']:s for s in surface_manifest['assets']}
    texture_sets={}
    for name in sorted({n for r in rows for n in r.get('measured',{}).get('texture_sets',[]) if n!='flat'}):
        folder=game/'demos/human_tech_demo/assets/textures'/name
        files=[]
        for file in sorted(folder.glob('*.jpg')):
            files.append(dict(file=str(file.relative_to(game)),bytes=file.stat().st_size,
                              sha256=hashlib.sha256(file.read_bytes()).hexdigest()))
        source=surface_sources.get(name,{})
        verified=bool(files) and all(source.get('files',{}).get(Path(f['file']).name)==f['sha256'] for f in files)
        texture_sets[name]=dict(files=files,file_bytes=sum(f['bytes'] for f in files),
            source_resolution=source.get('resolution') if verified else None,
            files_match_surface_manifest=verified,
            complete_map_files=all((folder/f).exists() for f in ('color.jpg','normal.jpg','roughness.jpg')))
    shared=[]
    for file in (arrival_kit,arrival_kit.with_suffix('.blend'),arrival_kit.with_suffix('.glb'),base_kit):
        shared.append(dict(file=str(file.relative_to(game)),bytes=file.stat().st_size,sha256=hashlib.sha256(file.read_bytes()).hexdigest()))
    revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=game,text=True).strip()
    catalog=dict(schema_version=1,title='Tower library — First Arrival',game_revision_at_inventory=revision,
                 units='metres, native Y-up',assets=rows,shared_files=shared,shared_texture_sets=texture_sets,missing_images=missing,
                 accounting='Triangles are native emitted triangles. Resource payload counts geometry once; shared kit/material/texture bytes are not assigned repeatedly to each model.')
    (output/'tower-inventory.json').write_text(json.dumps(catalog,indent=2)+'\n')
    esc=html.escape;parts=[]
    for group,label in GROUPS:
        part=[f'<tbody data-section="{group}"><tr class="section"><th colspan="3"><span>{esc(label)}</span></th></tr>']
        for record in (r for r in rows if r['group']==group):
            key=record['key'];folder=record['folder'];m=record.get('measured');image='assets/'+folder+'/full.png'
            if m:
                size=short_bytes(m['resource_payload_bytes'])
                triangles=f"{m['rendered_triangles']:,}"
                unique=f"{m['unique_triangles']:,}"
                dimensions=' × '.join(f'{n:.1f}' for n in m['dimensions_metres'])
                textures=[v for v in m['texture_sets'] if v!='flat']
                maps='No bitmap texture maps; native material factors and procedural shading.' if not textures else 'Shared surface sets: '+', '.join(textures)+'.'
                installed=[n for n in textures if texture_sets[n]['complete_map_files']]
                procedural=[n for n in textures if not texture_sets[n]['complete_map_files']]
                if installed:
                    descriptions=[]
                    for n in installed:
                        t=texture_sets[n];resolution=' × '.join(map(str,t['source_resolution'])) if t['source_resolution'] else 'resolution not verified'
                        descriptions.append(f'{n}: {len(t["files"])} installed JPG maps, {resolution}, {short_bytes(t["file_bytes"])} shared')
                    maps+=' Installed source textures: '+'; '.join(descriptions)+'.'
                if procedural:maps+=' Procedural/fallback surfaces (no installed bitmap files): '+', '.join(procedural)+'.'
                details=f'<dl><div><dt>Triangles</dt><dd>{triangles}</dd></div><div><dt>Unique triangles</dt><dd>{unique}</dd></div><div><dt>Geometry payload</dt><dd>{size}</dd></div><div><dt>Materials</dt><dd>{m["material_count"]}</dd></div><div><dt>Size · W × H × D</dt><dd>{dimensions} m</dd></div></dl><p class="small">{esc(maps)}</p>'
                if record['kind'] in ('kit','base-kit'):
                    details+='<p class="small">One resource inside a shared kit. File overhead, other models and shared materials are excluded from this payload.</p>'
                else:
                    details+='<p class="small">Generated on demand; payload is the exact diagnostic mesh export, not a separately shipped model file.</p>'
                picture=f'<button class="image-button" data-image="{image}" data-name="{esc(record["name"],quote=True)}"><img src="{image}" alt="Complete {esc(record["name"],quote=True)} studio render" loading="lazy"><span>View full image ↗</span></button>'
            else:details='<p>Awaiting final geometry export.</p>';picture='<div class="pending">Rendering pending</div>'
            model='arrival_towers.htkit · editable arrival_towers.blend' if record['kind']=='kit' else 'human_tech_kit.htkit' if record['kind']=='base-kit' else 'Generated from the source below; mesh.bin is a diagnostic export.'
            description=f'<span class="tag">{esc(group)}</span><h3>{esc(record["name"])}</h3><p>{esc(record["description"])}</p><p class="use"><strong>Use</strong> {esc(record["use"])}</p><p class="file"><strong>Resource / inventory selector</strong><code>{esc(key)}</code><strong>Model file</strong><code>{esc(model)}</code><strong>Source in game repository</strong><code>{esc(record["source"])}</code></p>'
            if record['note']:details+=f'<p class="note">{esc(record["note"])}</p>'
            part.append(f'<tr class="asset" data-group="{group}" data-search="{esc((record["name"]+" "+record["description"]+" "+key).lower(),quote=True)}"><td>{picture}</td><td>{description}</td><td>{details}</td></tr>')
        part.append('</tbody>');parts.extend(part)
    shared_html=''.join(f'<li><code>{esc(Path(r["file"]).name)}</code><strong>{short_bytes(r["bytes"])}</strong></li>' for r in shared)
    shared_html+=''.join(f'<li><code>textures/{esc(n)} · {len(t["files"])} JPG maps</code><strong>{short_bytes(t["file_bytes"])}</strong></li>' for n,t in texture_sets.items() if t['files'])
    document='''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Tower library · First Arrival</title>
<style>
:root{color-scheme:light;--ink:#202e31;--muted:#5c6b70;--line:#d6dfdc;--paper:#f5f5ef;--teal:#236d68;--soft:#e7edE8}*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:16px/1.55 system-ui,sans-serif}header,main,footer{max-width:1560px;margin:auto;padding:32px 48px}header{padding-top:64px;padding-bottom:26px}.eyebrow{font-size:12px;font-weight:750;letter-spacing:.18em;text-transform:uppercase;color:var(--teal)}h1{font-size:clamp(38px,5vw,68px);line-height:1.05;letter-spacing:-.05em;margin:18px 0}header p{max-width:880px;font-size:18px;color:var(--muted)}.stats{display:flex;gap:34px;flex-wrap:wrap;margin:28px 0}.stats b{font-size:28px;display:block;color:var(--ink)}.stats span{font-size:12px;letter-spacing:.06em;text-transform:uppercase;color:var(--muted)}.notice{padding:18px 22px;background:var(--soft);border-left:4px solid var(--teal);max-width:1120px;font-size:14px}.toolbar{display:flex;gap:12px;flex-wrap:wrap;align-items:center;padding:18px 0 26px;position:sticky;top:0;background:var(--paper);z-index:3}input,select{font:inherit;border:1px solid #b8c9c3;border-radius:8px;padding:12px 16px;color:var(--ink);background:#fff}input{flex:1;min-width:240px}#count{font-size:13px;color:var(--muted)}table{width:100%;border-collapse:collapse;table-layout:fixed}col:nth-child(1){width:30%}col:nth-child(2){width:39%}col:nth-child(3){width:31%}thead th{text-align:left;font-size:12px;letter-spacing:.10em;text-transform:uppercase;padding:15px 22px;background:var(--ink);color:white}.section th{text-align:left;font-size:22px;padding:36px 0 14px;letter-spacing:-.02em}.asset td{background:#fff;vertical-align:top;border-bottom:12px solid var(--paper);padding:28px 25px}.asset td:first-child{padding:0;border-right:1px solid var(--paper)}h3{font-size:25px;line-height:1.14;letter-spacing:-.035em;margin:13px 0 15px}p{margin:12px 0}.tag{font-size:10px;text-transform:uppercase;letter-spacing:.13em;background:var(--soft);color:var(--teal);padding:5px 8px;border-radius:4px}.image-button{display:block;width:100%;padding:0;border:0;background:#d8dcd8;cursor:zoom-in;color:#34434a}.image-button img{width:100%;height:445px;object-fit:contain;display:block}.image-button span{display:block;background:#edf0ec;font-size:11px;letter-spacing:.04em;padding:8px}.use{color:var(--muted);font-size:14px}.use strong{color:var(--teal);margin-right:5px}.file{margin-top:22px}.file strong{font-size:10px;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);display:block;margin-top:12px}code{font-family:ui-monospace,monospace;font-size:12px;word-break:break-word;display:block;line-height:1.65}dl{margin:2px 0 22px}dl div{display:flex;justify-content:space-between;gap:14px;border-bottom:1px solid var(--line);padding:9px 0}dt{color:var(--muted);font-size:12px}dd{margin:0;font-weight:650;font-size:14px;text-align:right}.small,.note{font-size:12px;color:var(--muted)}.note{border-left:2px solid var(--teal);padding-left:12px;margin-top:18px}.shared{padding:26px;background:var(--soft);margin-top:28px;border-radius:12px}.shared h2{font-size:20px;margin:0}.shared ul{list-style:none;padding:0;display:grid;grid-template-columns:1fr 1fr;gap:12px 36px}.shared li{display:flex;gap:20px;justify-content:space-between}.shared strong{white-space:nowrap;font-size:14px}a{color:var(--teal)}footer{font-size:12px;color:var(--muted);padding-top:0}dialog{padding:0;border:0;border-radius:10px;background:#222c31;max-width:95vw;max-height:96vh;color:white}dialog::backdrop{background:#0c151ce6}dialog img{display:block;max-height:86vh;max-width:95vw;width:auto}dialog header{padding:10px 15px;display:flex;justify-content:space-between;align-items:center;gap:25px;font-size:14px}dialog button{font:inherit;background:#fff2;border:0;color:white;padding:6px 12px;border-radius:5px;cursor:pointer}.pending{min-height:250px;display:grid;place-items:center;color:var(--muted)}tr[hidden],tbody[hidden]{display:none!important}
@media(max-width:950px){header,main,footer{padding-left:20px;padding-right:20px}.asset td{padding:20px 15px}.image-button img{height:350px}h3{font-size:21px}col:nth-child(1){width:29%}col:nth-child(2){width:38%}col:nth-child(3){width:33%}}
@media(max-width:650px){thead,colgroup{display:none}table,tbody{display:block}.asset{display:grid;grid-template-columns:1fr;width:100%}.asset td{display:block;border:0;padding:22px}.asset td:first-child{border:0}.asset td:last-child{border-bottom:16px solid var(--paper)}.image-button img{height:440px}.section{display:block}.section th{display:block}.shared ul{grid-template-columns:1fr}.toolbar{position:static}dl div{padding:7px 0}.stats{gap:22px}}
@media print{.toolbar,.image-button span{display:none}.asset{break-inside:avoid}.asset td{padding:15px}.image-button img{height:300px}header,main,footer{padding:15px}.notice{font-size:11px}}
</style>
<header><div class="eyebrow">unendlich · asset library · 11 September 2026</div><h1>Towers, up close.</h1>
<p>Every current authored tower, named landmark variant and reusable tower family in one illustrated inventory. Each image frames the complete listed asset, including its crown and lowest geometry.</p>
<div class="stats"><span><b>ASSET_COUNT</b>Inventory entries</span><span><b>6</b>Authored Arrival shafts</span><span><b>6</b>Occupied bases</span><span><b>1 : 1</b>Metre scale</span></div>
<div class="notice"><strong>How to read these images.</strong> These are exact native geometry exports rendered in a neutral Blender studio. They show form and construction clearly; they are not in-engine screenshots. Studio glass approximates the native pane factors. Native procedural room lighting, surface maps and foliage-alpha appearance differ. Counts exclude the diagnostic floor, camera and lights.</div></header>
<main><div class="toolbar"><input id="search" type="search" placeholder="Find a tower, family or resource…" aria-label="Search inventory"><select id="group" aria-label="Asset category"><option value="all">All entries</option>GROUP_OPTIONS</select><span id="count"></span></div>
<table><colgroup><col><col><col></colgroup><thead><tr><th>Complete asset render</th><th>Name, file &amp; use</th><th>Size, geometry &amp; notes</th></tr></thead>ROWS</table>
<section class="shared"><h2>Shared files — counted once</h2><p class="small">The six authored towers share one model library. Their per-row payloads are not six copies of these complete file sizes. Native generators have source recipes and diagnostic exports rather than unique installed model files.</p><ul>SHARED</ul><p class="small">“Polygons” means emitted triangles throughout. Unique triangles count shared meshes once; rendered triangles include every displayed instance. Width × height × depth uses the native Y-up bounds. Source paths are relative to the game repository.</p></section>
</main><footer><p><a href="tower-inventory.json">Measured inventory and material metadata</a> · <a href="README.md">Iteration review</a></p><p>Current assets and deterministic generator examples are included. Superseded six-tower drafts and earlier material candidates remain historical review evidence, not duplicate library assets. Reproduce with the game’s build-tower-inventory.py export/render workflow.</p></footer>
<dialog id="zoom"><header><span id="zoom-name"></span><button id="close">Close ×</button></header><img id="zoom-image" alt=""></dialog>
<script>
const search=document.querySelector('#search'),group=document.querySelector('#group'),count=document.querySelector('#count');
function filter(){let visible=0;document.querySelectorAll('tr.asset').forEach(row=>{row.hidden=!(row.dataset.search.includes(search.value.toLowerCase())&&(group.value==='all'||group.value===row.dataset.group));if(!row.hidden)visible++});document.querySelectorAll('tbody').forEach(section=>section.hidden=!section.querySelector('tr.asset:not([hidden])'));count.textContent=visible+' entries';}
search.addEventListener('input',filter);group.addEventListener('change',filter);filter();
const zoom=document.querySelector('#zoom');document.querySelectorAll('[data-image]').forEach(button=>button.addEventListener('click',()=>{document.querySelector('#zoom-image').src=button.dataset.image;document.querySelector('#zoom-image').alt=button.dataset.name;document.querySelector('#zoom-name').textContent=button.dataset.name;zoom.showModal()}));
document.querySelector('#close').onclick=()=>zoom.close();zoom.addEventListener('click',event=>{if(event.target===zoom)zoom.close()});
</script></html>'''
    document=document.replace('ASSET_COUNT',str(len(rows))).replace('GROUP_OPTIONS',''.join(f'<option value="{g}">{esc(label)}</option>' for g,label in GROUPS)).replace('ROWS',''.join(parts)).replace('SHARED',shared_html)
    (output/'tower-inventory.html').write_text(document)
    print('Inventory:',len(rows),'entries;',len(missing),'pending images;',output/'tower-inventory.html')

if __name__=='__main__':main()
