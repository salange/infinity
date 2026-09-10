# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Independent glTF accessor/material/transform rejection fixtures."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import sys
sys.dont_write_bytecode=True
import unittest
spec=importlib.util.spec_from_file_location('compiler',Path(__file__).resolve().parents[1]/'tools'/'compile-gltf-kit.py')
compiler=importlib.util.module_from_spec(spec);spec.loader.exec_module(compiler)

class CompilerContract(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.path=Path(self.tmp.name)
        self.blob=struct.pack('<9f9f12f6f3I',0,0,0,1,0,0,0,1,0, 0,0,1,0,0,1,0,0,1, 1,0,0,1,1,0,0,1,1,0,0,1, 0,0,1,0,0,1, 0,1,2)
        self.doc={'asset':{'version':'2.0'},'buffers':[{'byteLength':len(self.blob)}],
                  'bufferViews':[{'buffer':0,'byteOffset':o,'byteLength':n} for o,n in ((0,36),(36,36),(72,48),(120,24),(144,12))],
                  'accessors':[{'bufferView':i,'componentType':5125 if i==4 else 5126,'count':3,'type':t} for i,t in enumerate(('VEC3','VEC3','VEC4','VEC2','SCALAR'))],
                  'materials':[{'name':'ivory','pbrMetallicRoughness':{'baseColorFactor':[.8,.7,.6,1],'metallicFactor':0,'roughnessFactor':.4}}],
                  'meshes':[{'primitives':[{'attributes':dict(POSITION=0,NORMAL=1,TANGENT=2,TEXCOORD_0=3),'indices':4,'material':0}]}],
                  'nodes':[{'name':'fixture','extras':{'resource':True},'children':[1],'translation':[2,3,4]}, {'mesh':0,'scale':[2,3,4]}],
                  'scenes':[{'nodes':[0]}],'scene':0}
    def tearDown(self):self.tmp.cleanup()
    def write_source(self):
        d=json.dumps(self.doc).encode();d+=b' '*(-len(d)%4);b=self.blob+b'\0'*(-len(self.blob)%4)
        raw=struct.pack('<III',0x46546c67,2,12+8+len(d)+8+len(b))+struct.pack('<II',len(d),0x4e4f534a)+d+struct.pack('<II',len(b),0x004e4942)+b
        source=self.path/'fixture.glb';source.write_bytes(raw)
        return source
    def compile(self):
        return compiler.compile_kit(self.write_source(),self.path/'fixture.htkit')
    def canonicalize(self):
        canonical_spec=importlib.util.spec_from_file_location('canonical',Path(__file__).resolve().parents[1]/'tools'/'canonicalize-gltf-kit.py')
        canonical=importlib.util.module_from_spec(canonical_spec);canonical_spec.loader.exec_module(canonical)
        result=self.path/'canonical.glb';canonical.canonicalize(self.write_source(),result)
        return compiler.Gltf(result)
    def test_transform_and_linear_material(self):
        r=self.compile();self.assertEqual(r['resources'][0]['bounds_m_y_up'],[[2,3,4],[4,6,4]])
        self.assertEqual(r['materials'][0]['base_color_linear'],[.8,.7,.6])
    def test_missing_tangent_rejected(self):
        del self.doc['meshes'][0]['primitives'][0]['attributes']['TANGENT']
        with self.assertRaisesRegex(ValueError,'missing required'):self.compile()
    def test_unmapped_texture_rejected(self):
        self.doc['materials'][0]['pbrMetallicRoughness']['baseColorTexture']={'index':0}
        with self.assertRaisesRegex(ValueError,'texture map'):self.compile()
    def test_malformed_accessor_rejected(self):
        self.doc['accessors'][0]['count']=100
        with self.assertRaisesRegex(ValueError,'accessor exceeds'):self.compile()
    def test_unknown_extension_rejected(self):
        self.doc['extensionsRequired']=['UNSUPPORTED_example']
        with self.assertRaisesRegex(ValueError,'unsupported required'):self.compile()
    def test_double_sided_and_mirror(self):
        self.doc['materials'][0]['doubleSided']=True;self.doc['nodes'][1]['scale']=[-2,3,4]
        r=self.compile();self.assertEqual(r['resources'][0]['triangles'],2)
        self.assertEqual(r['resources'][0]['bounds_m_y_up'],[[0,3,4],[2,6,4]])
    def test_reproducible_compilation(self):
        a=self.compile();b=self.compile();self.assertEqual(a['runtime_sha256'],b['runtime_sha256'])

    def test_glass_factors_retained(self):
        material=self.doc['materials'][0]
        material['alphaMode']='BLEND';material['pbrMetallicRoughness']['baseColorFactor'][3]=.18
        material['extensions']={'KHR_materials_transmission':{'transmissionFactor':.86}}
        r=self.compile();self.assertEqual(r['materials'][0]['glass_optical'],{'ior':1.5,'transmission':.86,'alpha':.18,'thickness_m':.012})
    def test_authored_occupied_room_factors_retained(self):
        self.doc['materials'][0]['extras']={'engine_flags':1,'engine_room':[3.2,4.125,7,.28],
                                          'engine_tint2':[.2,.3,.4],'engine_normal_strength':.06}
        r=self.compile();m=r['materials'][0]
        self.assertEqual(m['occupied_room'],dict(width_m=3.2,height_m=4.125,depth_m=7,lit_probability=.28))
        self.assertIsNone(m['glass_optical'])
        raw=(self.path/'fixture.htkit').read_bytes();offset=148+4+len('ivory')
        factors=struct.unpack_from('<7fI7f',raw,offset)
        self.assertAlmostEqual(factors[6],.06)
        self.assertAlmostEqual(factors[12],4.125)
        self.assertAlmostEqual(factors[8],.2)
    def test_invalid_occupied_material_rejected(self):
        self.doc['materials'][0]['extras']={'engine_flags':1,'engine_room':[3.2,0,7,.28]}
        with self.assertRaisesRegex(ValueError,'occupied room'):self.compile()
        self.doc['materials'][0]['extras']={'engine_flags':129}
        with self.assertRaisesRegex(ValueError,'distinct material'):self.compile()
    def test_zero_tangent_reconstructed_from_uv(self):
        canonical_spec=importlib.util.spec_from_file_location('canonical',Path(__file__).resolve().parents[1]/'tools'/'canonicalize-gltf-kit.py')
        canonical=importlib.util.module_from_spec(canonical_spec);canonical_spec.loader.exec_module(canonical)
        data=bytearray(self.blob);data[72:84]=struct.pack('<3f',0,0,0);self.blob=bytes(data)
        with self.assertRaisesRegex(ValueError,'degenerate normal/tangent'):self.compile()
        result=self.path/'repaired.glb';canonical.canonicalize(self.path/'fixture.glb',result)
        report=compiler.compile_kit(result,self.path/'repaired.htkit')
        self.assertEqual(report['resources'][0]['triangles'],1)
    def test_occupied_basis_follows_metric_uv_after_export(self):
        self.doc['materials'][0]['extras']={'engine_flags':1}
        for mirrored in (False,True):
            self.doc['nodes'][1]['scale']=[-2 if mirrored else 2,3,4]
            for uv_direction in (-1.,1.):
                for exported_sign in (-1.,1.):
                    with self.subTest(mirrored=mirrored,uv_direction=uv_direction,exported_sign=exported_sign):
                        data=bytearray(self.blob)
                        for offset in (84,100,116):struct.pack_into('<f',data,offset,exported_sign)
                        struct.pack_into('<f',data,140,uv_direction)
                        self.blob=bytes(data);g=self.canonicalize()
                        attrs=g.doc['meshes'][0]['primitives'][0]['attributes']
                        normals=g.accessor(attrs['NORMAL']);tangents=g.accessor(attrs['TANGENT'])
                        for n,t in zip(normals,tangents):
                            upward=(n[2]*t[0]-n[0]*t[2])*t[3]
                            self.assertAlmostEqual(upward,uv_direction)
    def test_nonoccupied_and_textured_tangent_signs_preserved(self):
        cases=({'extras':{'engine_flags':0}},
               {'extras':{'engine_flags':128}},
               {'extras':{'engine_flags':129}},
               {'extras':{'engine_flags':1},'normalTexture':{'index':0}})
        data=bytearray(self.blob)
        for offset in (84,100,116):struct.pack_into('<f',data,offset,-1.)
        self.blob=bytes(data)
        for material_fields in cases:
            with self.subTest(material_fields=material_fields):
                self.doc['materials'][0]={'name':'fixture',**material_fields}
                g=self.canonicalize();attrs=g.doc['meshes'][0]['primitives'][0]['attributes']
                self.assertEqual([t[3] for t in g.accessor(attrs['TANGENT'])],[-1.,-1.,-1.])
    def test_canonical_export_ignores_vertex_order(self):
        canonical_spec=importlib.util.spec_from_file_location('canonical',Path(__file__).resolve().parents[1]/'tools'/'canonicalize-gltf-kit.py')
        canonical=importlib.util.module_from_spec(canonical_spec);canonical_spec.loader.exec_module(canonical)
        self.compile();a=self.path/'first.glb';canonical.canonicalize(self.path/'fixture.glb',a)
        data=bytearray(self.blob)
        for offset,width in ((0,12),(36,12),(72,16),(120,8)):
            old=self.blob[offset:offset+width*3]
            data[offset:offset+width*3]=old[width:]+old[:width]
        data[144:156]=struct.pack('<3I',2,0,1);self.blob=bytes(data)
        self.compile();b=self.path/'second.glb';canonical.canonicalize(self.path/'fixture.glb',b)
        self.assertEqual(a.read_bytes(),b.read_bytes())

if __name__=='__main__':unittest.main()
