#!/usr/bin/env python3
"""Turn the CRT shaders in yabause/src/crt/shaders/ (as RetroArch ships them,
unmodified) into C strings for the renderers: yabause/src/crt/crt_shaders.h.

Two variants of every pass are written:

OpenGL (crt_gl.c): the body without a #version line. The renderer puts
"#version 330" (desktop) or "#version 300 es" plus precision statements
(OpenGL ES) in front of it at run time.
  - RetroArch's GLSL shaders (glsl-shaders) compile as they are once VERTEX or
    FRAGMENT and PARAMETER_UNIFORM are defined.
  - Its slang shaders (slang-shaders) are Vulkan GLSL: the push constant and
    UBO blocks become plain uniforms, the layout qualifiers go and FrameCount
    becomes a float.
  - For GLSL ES 3.00, which has no implicit int-to-float conversion and only
    constant initialisers for globals: a global initialised from uniforms
    becomes a #define, and integer literals mixed with the float FrameCount
    become float literals. The arithmetic is the same on desktop GL.
  - A #define of gl_FragCoord (a reserved name in GLSL ES) is renamed.

Vulkan (#version 450, for runtime compilation with shaderc):
  - slang passes are used as RetroArch uses them: push constants, the MVP in a
    UBO at set 0 binding 0, samplers at the bindings the file gives.
  - GLSL passes: every non-sampler uniform (MVPMatrix, sizes, frame counters
    and the parameters) goes into one std140 UBO at set 0 binding 0, the
    input sampler goes to binding 1, and the inputs and outputs get explicit
    locations (vertex inputs: 0 position, 1 texture coordinate, 2 colour;
    varyings: 0 texture coordinate, 1 colour). The member offsets are written
    out in a table.
  - A fragment shader that reads gl_FragCoord (the masks of Hyllian and
    Lottes) gets it as OpenGL sees it in the window the user looks at:
    counted from the bottom, and with the swapchain pre-transform undone
    (the renderer draws pre-rotated into a turned framebuffer). The
    framebuffer size and the pre-transform's quarter turns come in a UBO at
    set 0 binding 15 (CrtDisplay). A pass that #defines gl_FragCoord itself
    is left alone.

Each pass keeps its #pragma parameter defaults.

usage: python yabause/src/crt/crt_embed.py [--out PATH]"""
import argparse
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'shaders')
OUT = os.path.join(HERE, 'crt_shaders.h')


def params_of(src):
    return [(m.group(1), float(m.group(2)))
            for m in re.finditer(r'^#pragma parameter (\w+)\s+"[^"]*"\s+([-\d.]+)', src, flags=re.M)]


def globals_to_defines(src):
    """A global (column 0) 'type name = value;' becomes '#define name (value)'."""
    def repl(m):
        value = ' '.join(m.group(3).split())
        return '#define %s (%s)' % (m.group(2), value)
    return re.sub(r'^(float|vec2|vec3|vec4|mat4)\s+(\w+)\s*=\s*(.*?);', repl, src, flags=re.M | re.S)


# --- std140 / std430 offsets for the block tables ---------------------------
SIZES = {'float': (4, 4), 'int': (4, 4), 'uint': (4, 4), 'vec2': (8, 8), 'vec4': (16, 16), 'mat4': (64, 16)}


def layout(members):
    out, off = [], 0
    for typ, name in members:
        size, align = SIZES[typ]
        off = (off + align - 1) // align * align
        out.append((name, typ, off))
        off += size
    return out, off


# --- GLSL (glsl-shaders) -------------------------------------------------------
def from_glsl(path):
    src = open(path, encoding='utf-8').read()
    params = params_of(src)
    body = re.sub(r'^#version.*$', '', src, flags=re.M)
    body = globals_to_defines(body)
    head = '#define PARAMETER_UNIFORM\n'
    gl_vs = head + '#define VERTEX\n' + body
    gl_fs = head + '#define FRAGMENT\n' + body

    # Vulkan: one UBO with every non-sampler uniform, in first-seen order
    members, seen = [], set()
    uni = re.compile(r'^[ \t]*uniform\s+(?:PRECISION\s+|COMPAT_PRECISION\s+)?(\w+)\s+(\w+)\s*;[ \t]*$', re.M)
    for m in uni.finditer(body):
        typ, name = m.group(1), m.group(2)
        if typ.startswith('sampler'):
            continue
        if name not in seen:
            seen.add(name)
            members.append((typ, name))

    def drop_uniform(m):
        return m.group(0) if m.group(1).startswith('sampler') else ''
    vk = uni.sub(drop_uniform, body)
    vk = re.sub(r'^([ \t]*)uniform\s+sampler2D\s+(\w+)\s*;', r'\1layout(set = 0, binding = 1) uniform sampler2D \2;', vk, flags=re.M)
    vin = {'VertexCoord': 0, 'TexCoord': 1, 'Color': 2, 'COLOR': 2}
    vary = {'TEX0': 0, 'texCoord': 0, 'COL0': 1, 'color': 1}

    def loc(m):
        q, typ, name = m.group(2), m.group(3), m.group(4)
        table = vin if q in ('COMPAT_ATTRIBUTE',) or (q == 'IN' and name in vin) else vary
        if name not in table:
            raise SystemExit('%s: no location for %s' % (path, name))
        return '%slayout(location = %d) %s %s %s;' % (m.group(1), table[name], q, typ, name)
    vk = re.sub(r'^([ \t]*)(IN|OUT|COMPAT_ATTRIBUTE|COMPAT_VARYING)\s+(\w+)\s+(\w+)\s*;', loc, vk, flags=re.M)
    vk = re.sub(r'^out vec4 FragColor;', 'layout(location = 0) out vec4 FragColor;', vk, flags=re.M)
    lay, size = layout(members)
    block = 'layout(std140, set = 0, binding = 0) uniform UBO\n{\n' + \
        ''.join('    %s %s;\n' % (t, n) for n, t, _ in lay) + '};\n'
    vk_head = '#version 450\n#define PARAMETER_UNIFORM\n'
    vk_vs = vk_head + '#define VERTEX\n' + block + vk
    vk_fs = vk_head + '#define FRAGMENT\n' + block + vk
    return dict(gl_vs=gl_vs, gl_fs=gl_fs, vk_vs=vk_vs, vk_fs=vk_fs, params=params,
                slang=0, block=lay, block_size=size, block_push=0)


# --- slang (slang-shaders) -----------------------------------------------------
def from_slang(path):
    src = open(path, encoding='utf-8').read()
    params = params_of(src)
    src = re.sub(r'^#pragma parameter.*$', '', src, flags=re.M)

    # Vulkan: as RetroArch compiles it, one stage at a time
    vk = re.sub(r'^#version.*$', '', src, flags=re.M)
    common, rest = vk.split('#pragma stage vertex')
    vs, fs = rest.split('#pragma stage fragment')
    vk_vs = '#version 450\n' + common + vs
    vk_fs = '#version 450\n' + common + fs
    push = re.search(r'layout\(push_constant\)\s*uniform\s+\w+\s*\{(.*?)\}', src, flags=re.S)
    members = []
    for member in push.group(1).split(';'):
        member = member.split()
        if member:
            members.append((member[0], member[1]))
    lay, size = layout(members)   # push constants: std430, the same as std140 for these types

    # OpenGL
    def block(m):
        out = []
        for member in m.group(1).split(';'):
            member = member.strip()
            if member:
                out.append('uniform ' + member.replace('uint ', 'float ') + ';')
        return '\n'.join(out)
    gl = re.sub(r'layout\([^)]*\)\s*uniform\s+\w+\s*\{(.*?)\}\s*\w+;', block, src, flags=re.S)
    gl = gl.replace('params.', '').replace('global.', '')
    gl = globals_to_defines(gl)
    gl = re.sub(r'layout\([^)]*\)\s*', '', gl)
    gl = re.sub(r'^#version.*$', '', gl, flags=re.M)
    # FrameCount is a float here: no int operands next to it (GLSL ES)
    gl = re.sub(r'mod\(FrameCount, (\d+)\) \* (\d+)\)', r'mod(FrameCount, \1.0) * \2.0)', gl)
    # gl_FragCoord may not be #defined in GLSL ES
    if '#define gl_FragCoord' in gl:
        gl = re.sub(r'\bgl_FragCoord\b', 'crt_FragCoord', gl)
    common, rest = gl.split('#pragma stage vertex')
    vs, fs = rest.split('#pragma stage fragment')
    return dict(gl_vs=common + vs, gl_fs=common + fs, vk_vs=vk_vs, vk_fs=vk_fs, params=params,
                slang=1, block=lay, block_size=size, block_push=1)


# Vulkan's gl_FragCoord counts from the top of the framebuffer, which is the
# physical one: turned by the pre-transform. crt_fc = framebuffer w, h,
# pre-transform quarter turns (pre_rotate_mat's direction), unused.
DISPLAY_FRAGCOORD = """// CRT mode: gl_FragCoord as OpenGL sees it in the displayed window
layout(std140, set = 0, binding = 15) uniform CrtDisplay { ivec4 crt_fc; };
vec4 crt_display_fragcoord()
{
    vec2 p = gl_FragCoord.xy;
    vec2 s = vec2(crt_fc.xy);
    int k = crt_fc.z & 3;
    vec2 d = (k == 1) ? vec2(p.y, p.x) :
             (k == 2) ? vec2(s.x - p.x, p.y) :
             (k == 3) ? vec2(s.y - p.y, s.x - p.x) :
                        vec2(p.x, s.y - p.y);
    return vec4(d, gl_FragCoord.zw);
}
#define gl_FragCoord crt_display_fragcoord()
"""


def display_fragcoord(vk_fs):
    if 'gl_FragCoord' not in vk_fs or '#define gl_FragCoord' in vk_fs:
        return vk_fs
    head, rest = vk_fs.split(chr(10), 1)
    assert head == '#version 450'
    return head + chr(10) + DISPLAY_FRAGCOORD + rest


def c_string(text):
    lines = []
    for line in text.split('\n'):
        line = line.rstrip()
        lines.append('    "' + line.replace('\\', '\\\\').replace('"', '\\"') + '\\n"')
    return '\n'.join(lines)


# (table, [(file, linear, scale_source)]): the presets' filter_linear and scale_type
SHADERS = [
    ('crt_hyllian_passes', [('crt-hyllian.glsl', 0, 0)]),
    ('crt_lottes_passes', [('crt-lottes.glsl', 0, 0)]),
    ('crt_newpixie_passes', [('newpixie/accumulate.slang', 1, 1), ('newpixie/blur_horiz.slang', 1, 1),
                             ('newpixie/blur_vert.slang', 1, 1), ('newpixie/newpixie-crt.slang', 1, 0)]),
]


def main():
    ap = argparse.ArgumentParser(description='Generate crt_shaders.h from the RetroArch CRT shaders in shaders/.')
    ap.add_argument('--out', default=OUT, help='output header (default: %(default)s)')
    args = ap.parse_args()
    out = ['/* Generated by yabause/src/crt/crt_embed.py from yabause/src/crt/shaders/ (see',
           ' * README.md there); do not edit. RetroArch CRT shaders for the OpenGL renderer',
           ' * (crt_gl.c; the strings have no #version line) and the Vulkan renderer',
           ' * (#version 450, complete). */',
           '#ifndef YAB_CRT_SHADERS_H', '#define YAB_CRT_SHADERS_H', '',
           'typedef struct { const char* name; float value; } CrtParam;',
           '/* a member of the Vulkan uniform block (GLSL passes) or push constant block (slang passes) */',
           'typedef struct { const char* name; const char* type; int offset; } CrtBlockMember;',
           'typedef struct {',
           '    const char* vs; const char* fs;          /* OpenGL: put a #version line in front */',
           '    const char* vk_vs; const char* vk_fs;    /* Vulkan GLSL 450 */',
           '    const CrtParam* params; int nparams;     /* the #pragma parameter defaults */',
           '    int linear;                              /* the input is read with linear filtering (the preset\'s filter_linear) */',
           '    int scale_source;                        /* 1: as large as its input; 0: the output rectangle in the window */',
           '    int slang;                               /* 1: slang uniforms (vec4 sizes, FrameCount float in GL); 0: RetroArch GLSL */',
           '    const CrtBlockMember* block; int nblock; int block_size;',
           '    int block_push;                          /* Vulkan: 1 the block is push constants, 0 a UBO at set 0 binding 0 */',
           '} CrtPass;', '']
    for table, passes in SHADERS:
        entries = []
        for i, (name, linear, scale_source) in enumerate(passes):
            path = os.path.join(SRC, name)
            p = (from_slang if name.endswith('.slang') else from_glsl)(path)
            ident = f'{table}_{i}'
            out.append(f'/* {name} */')
            p['vk_fs'] = display_fragcoord(p['vk_fs'])
            for k in ('gl_vs', 'gl_fs', 'vk_vs', 'vk_fs'):
                out.append(f'static const char {ident}_{k}[] =\n{c_string(p[k])};')
            out.append(f'static const CrtParam {ident}_params[] = {{')
            out.extend(f'    {{"{n}", {v!r}f}},' for n, v in p['params'])
            if not p['params']:
                out.append('    {0, 0.0f},')
            out.append('};')
            out.append(f'static const CrtBlockMember {ident}_block[] = {{')
            out.extend(f'    {{"{n}", "{t}", {o}}},' for n, t, o in p['block'])
            out.append('};')
            entries.append(f'    {{{ident}_gl_vs, {ident}_gl_fs, {ident}_vk_vs, {ident}_vk_fs, {ident}_params, '
                           f'{len(p["params"])}, {linear}, {scale_source}, {p["slang"]}, {ident}_block, '
                           f'{len(p["block"])}, {p["block_size"]}, {p["block_push"]}}},')
        out.append(f'static const CrtPass {table}[{len(passes)}] = {{')
        out.extend(entries)
        out.append('};')
        out.append('')
    out.append('#endif')
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    print('wrote', args.out)


if __name__ == '__main__':
    main()
