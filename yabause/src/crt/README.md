# CRT mode shaders

The `shaders/` directory holds RetroArch CRT shaders exactly as they were
downloaded (not modified). `python yabause/src/crt/crt_embed.py` turns them
into C strings in `crt_shaders.h` (the generated file is committed):

- OpenGL / OpenGL ES (`crt_gl.c`): GLSL bodies without a `#version` line; the
  renderer prepends `#version 330` or `#version 300 es` + precision statements.
- Vulkan: complete `#version 450` sources for runtime compilation (shaderc),
  with the parameters in a uniform buffer (GLSL passes) or push constants
  (slang passes). The block member offsets are in the generated tables.

The mechanical conversions the script makes are listed at the top of
`crt_embed.py`. The picture's place in the window (4:3, a whole number of
window pixels per scan line, centred; one field's lines for interlace) is
`crt_layout.c`.

Fetched: 2026-10-17

| File | Source | Author | License |
|---|---|---|---|
| `shaders/crt-hyllian.glsl` | https://raw.githubusercontent.com/libretro/glsl-shaders/master/crt/shaders/hyllian/crt-hyllian.glsl | Copyright (C) 2011-2020 Hyllian - sergiogdb@gmail.com | MIT (full text at the top of the file) |
| `shaders/crt-lottes.glsl` | https://raw.githubusercontent.com/libretro/glsl-shaders/master/crt/shaders/crt-lottes.glsl | Timothy Lottes | Public domain ("PUBLIC DOMAIN CRT STYLED SCAN-LINE SHADER") |
| `shaders/newpixie/accumulate.slang`, `blur_horiz.slang`, `blur_vert.slang`, `newpixie-crt.slang` | https://raw.githubusercontent.com/libretro/slang-shaders/master/crt/shaders/newpixie/ | Copyright (c) 2016 Mattias Gustavsson, adapted for slang by hunterk | MIT or Public Domain (Unlicense), at the user's choice; MIT is chosen here |

Hyllian's mask code (`mask_weights`) is by hunterk, public domain (noted in
the file).

NewPixie's frame image (`crtframe.png`) is not included; its `use_frame`
parameter defaults to 0.

## Preset values

| Shader | Preset | Passes | filter_linear | scale_type |
|---|---|---|---|---|
| Hyllian | `glsl-shaders/crt/crt-hyllian.glslp` | 1 | false | (default: viewport) |
| Lottes | `glsl-shaders/crt/crt-lottes.glslp` | 1 | false | (default: viewport) |
| NewPixie | `slang-shaders/crt/newpixie-crt.slangp` | accumulate, blur_horiz, blur_vert | true | source x1 |
| | | newpixie-crt | true | viewport |

## License notices

### crt-hyllian.glsl (MIT)

```
Copyright (C) 2011-2020 Hyllian - sergiogdb@gmail.com

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### crt-lottes.glsl (public domain)

```
PUBLIC DOMAIN CRT STYLED SCAN-LINE SHADER
  by Timothy Lottes
```

### newpixie (MIT, chosen of MIT / Unlicense)

```
newpixie CRT
by Mattias Gustavsson
adapted for slang by hunterk

MIT License
Copyright (c) 2016 Mattias Gustavsson
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
