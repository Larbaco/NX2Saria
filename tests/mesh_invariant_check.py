# needs: clang, glslangValidator, spirv-val, spirv-as
"""Mesh positions made Invariant (runtime/vk/vk_pipeline.c spirv_mesh_invariant): builds the function, cut from
vk_pipeline.c by its text, and runs it on modules made here:

  - a mesh shader whose position is a member of the per-vertex block (glslang's gl_MeshVerticesEXT);
  - a mesh shader whose position is an array variable of its own, as tools/xlate/MeshSpirv.cs declares it (assembled);
  - the same block mesh shader with the position already invariant, and a vertex shader.

Checks that the two mesh shaders get one Invariant decoration on the position (OpMemberDecorate on the block member,
OpDecorate on the variable), that the rewritten modules pass spirv-val, that a second pass changes nothing, and that
the already-invariant mesh shader and the vertex shader are left alone. No game files.

Usage: python tests/mesh_invariant_check.py [--cc <clang>]
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

MESH_BLOCK = '''#version 460
#extension GL_EXT_mesh_shader : require
layout(local_size_x = 32) in;
layout(triangles, max_vertices = 3, max_primitives = 1) out;
void main() {
    SetMeshOutputsEXT(3, 1);
    if (gl_LocalInvocationIndex < 3)
        gl_MeshVerticesEXT[gl_LocalInvocationIndex].gl_Position = vec4(float(gl_LocalInvocationIndex), 0.0, 0.5, 1.0);
    if (gl_LocalInvocationIndex == 0) gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);
}
'''
MESH_INVARIANT = MESH_BLOCK.replace('layout(triangles', 'out gl_MeshPerVertexEXT { invariant vec4 gl_Position; } '
                                    'gl_MeshVerticesEXT[];\nlayout(triangles')
VERT = '''#version 460
void main() { gl_Position = vec4(float(gl_VertexIndex), 0.0, 0.5, 1.0); }
'''
# A position array of its own (MeshSpirv.cs: an Output variable of vec4[max vertices] decorated BuiltIn Position)
MESH_VARIABLE = '''OpCapability MeshShadingEXT
OpExtension "SPV_EXT_mesh_shader"
OpMemoryModel Logical GLSL450
OpEntryPoint MeshEXT %main "main" %pos %idx
OpExecutionMode %main LocalSize 32 1 1
OpExecutionMode %main OutputVertices 3
OpExecutionMode %main OutputPrimitivesEXT 1
OpExecutionMode %main OutputTrianglesEXT
OpDecorate %pos BuiltIn Position
OpDecorate %idx BuiltIn PrimitiveTriangleIndicesEXT
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%uint = OpTypeInt 32 0
%v3u = OpTypeVector %uint 3
%u0 = OpConstant %uint 0
%u1 = OpConstant %uint 1
%u2 = OpConstant %uint 2
%u3 = OpConstant %uint 3
%arr = OpTypeArray %v4 %u3
%parr = OpTypePointer Output %arr
%pos = OpVariable %parr Output
%iarr = OpTypeArray %v3u %u1
%piarr = OpTypePointer Output %iarr
%idx = OpVariable %piarr Output
%pv4 = OpTypePointer Output %v4
%pv3u = OpTypePointer Output %v3u
%f0 = OpConstant %float 0
%f1 = OpConstant %float 1
%vec = OpConstantComposite %v4 %f0 %f0 %f0 %f1
%tri = OpConstantComposite %v3u %u0 %u1 %u2
%main = OpFunction %void None %fn
%l = OpLabel
OpSetMeshOutputsEXT %u3 %u1
%p0 = OpAccessChain %pv4 %pos %u0
OpStore %p0 %vec
%i0 = OpAccessChain %pv3u %idx %u0
OpStore %i0 %tri
OpReturn
OpFunctionEnd
'''

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { SPV_OP_ENTRY_POINT = 15, SPV_OP_FUNCTION = 54, SPV_OP_DECORATE = 71, SPV_DEC_BUILTIN = 11 };
@BODY@
static uint32_t *rd(const char *p, uint64_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) exit(2);
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)l);
    if (fread(w, 1, (size_t)l, f) != (size_t)l) exit(2);
    fclose(f);
    *n = (uint64_t)l / 4;
    return w;
}
int main(int argc, char **argv) {   // in.spv out.spv: prints "changed again" (1/0 each), writes the rewrite
    (void)argc;
    uint64_t n, on = 0, again_n = 0;
    uint32_t *w = rd(argv[1], &n);
    uint32_t *o = spirv_mesh_invariant(w, n, &on);
    uint32_t *again = o ? spirv_mesh_invariant(o, on, &again_n) : NULL;
    printf("%d %d\n", o != NULL, again != NULL);
    if (o) {
        FILE *f = fopen(argv[2], "wb");
        fwrite(o, 4, on, f);
        fclose(f);
    }
    return 0;
}
'''


def invariants(spv):
    """(OpDecorate Invariant count, OpMemberDecorate Invariant count) of a module."""
    w = struct.unpack('<%dI' % (len(spv) // 4), spv)
    dec = mem = 0
    i = 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xFFFF
        if op == 71 and n >= 3 and w[i + 2] == 18:
            dec += 1
        if op == 72 and n >= 4 and w[i + 3] == 18:
            mem += 1
        i += n
    return dec, mem


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', help='the C compiler (default: MSYS2 UCRT64 clang on Windows, clang elsewhere)')
    args = ap.parse_args()
    win = os.name == 'nt'
    tool = lambda name: str(MSYS_BIN / f'{name}.exe') if win and MSYS_BIN.exists() else shutil.which(name)
    cc = args.cc or (str(MSYS_BIN / 'clang.exe') if win and MSYS_BIN.exists() else 'clang')
    glslang, val, asm = tool('glslangValidator'), tool('spirv-val'), tool('spirv-as')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    src = (ROOT / 'runtime' / 'vk' / 'vk_pipeline.c').read_text(encoding='utf-8')
    body = src[src.index('// Ocarina draws a fog sheet twice'):src.index('// Maxwell computes in IEEE 754')]
    fails = 0
    with tempfile.TemporaryDirectory(prefix='nx2saria-mesh-inv-') as tmp:
        t = Path(tmp)
        for name, text, stage in (('block', MESH_BLOCK, 'mesh'), ('invariant', MESH_INVARIANT, 'mesh'), ('vert', VERT, 'vert')):
            (t / f'{name}.{stage}').write_text(text)
            subprocess.check_call([glslang, '-V', '--target-env', 'vulkan1.3', '-o', str(t / f'{name}.spv'),
                                   str(t / f'{name}.{stage}')], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        (t / 'variable.spvasm').write_text(MESH_VARIABLE)
        subprocess.check_call([asm, '--target-env', 'spv1.4', '-o', str(t / 'variable.spv'), str(t / 'variable.spvasm')],
                              env=env)
        (t / 'check.c').write_text(HARNESS.replace('@BODY@', body), encoding='utf-8')
        exe = t / ('check.exe' if win else 'check')
        subprocess.check_call([cc, '-O2', '-Wall', '-Wextra', '-Werror', str(t / 'check.c'), '-o', str(exe)], env=env)
        # name: (changed, the Invariant decorations wanted after the rewrite as (OpDecorate, OpMemberDecorate))
        cases = {'block': (1, (0, 1)), 'variable': (1, (1, 0)), 'invariant': (0, None), 'vert': (0, None)}
        for name, (want_changed, want_inv) in cases.items():
            src_spv = t / f'{name}.spv'
            out = t / f'out_{name}.spv'
            r = subprocess.run([str(exe), str(src_spv), str(out)], capture_output=True, text=True, env=env)
            changed, again = (int(x) for x in r.stdout.split())
            before = invariants(src_spv.read_bytes())
            if changed != want_changed or again:
                print(f'FAIL {name}: changed {changed} (want {want_changed}), changed again {again}')
                fails += 1
                continue
            if not changed:
                continue
            got = invariants(out.read_bytes())
            if before != (0, 0) or got != want_inv:
                print(f'FAIL {name}: Invariant decorations {before} before, {got} after (want {want_inv})')
                fails += 1
            v = subprocess.run([val, '--target-env', 'vulkan1.3', str(out)], capture_output=True, text=True)
            if v.returncode:
                print(f'FAIL {name}: spirv-val: {(v.stdout + v.stderr).strip()}')
                fails += 1
    print('mesh invariant: ok' if not fails else f'mesh invariant: {fails} failures')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
