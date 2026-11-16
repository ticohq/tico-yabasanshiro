/*  Copyright 2026 devMiyax(smiyaxdev@gmail.com)

    This file is part of YabaSanshiro.

    YabaSanshiro is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    YabaSanshiro is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with YabaSanshiro; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

/*
 * CRT mode, OpenGL / OpenGL ES. The Saturn picture, rendered at its own
 * resolution, goes through one of RetroArch's CRT shaders (crt_shaders.h)
 * with as many whole window pixels per scan line as fit (crt_layout.c).
 *
 * First the lines of the frame, or those of the field shown this frame when
 * the picture is double-density interlaced, are copied to a texture of their
 * own, top line in row 0, as RetroArch hands a shader its input. Then each
 * pass of the shader draws into the next one's input and the last pass into
 * the window. The uniforms are the ones RetroArch gives: SourceSize /
 * TextureSize / InputSize, OriginalSize, OutputSize, FrameCount, MVP and the
 * #pragma parameter defaults. NewPixie also reads the previous frame of its
 * second pass (PassFeedback1) and this frame's first and third passes.
 */
#include <stdio.h>
#include <string.h>

#include "../ygl.h"
#include "crt_gl.h"
#include "crt_layout.h"
#include "crt_shaders.h"

#if defined(_OGLES3_)
static const char crt_version[] =
    "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n";
#else
static const char crt_version[] = "#version 330\n";
#endif

/* the lines of a field: row r of the target is line r * step + field, and the
   source has line 0 in its top row */
static const char crt_cut_vs[] =
    "in vec2 a_pos;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
static const char crt_cut_fs[] =
    "uniform sampler2D u_img;\n"
    "uniform ivec4 u_cut;\n"   /* top row of the source, step, field, unused */
    "out vec4 o_color;\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  o_color = vec4(texelFetch(u_img, ivec2(p.x, u_cut.x - (p.y * u_cut.y + u_cut.z)), 0).rgb, 1.0);\n"
    "}\n";

#define CRT_SHADERS 3
#define CRT_PASSES 4

typedef struct {
  const CrtPass* pass;
  GLuint prog;
  GLint pos_at, tex_at, col_at;
  GLuint tex[2], fbo[2];   /* this frame's output and the last one (PassFeedback) */
  int w, h;
} CrtStage;

static const struct { const CrtPass* passes; int n; } crt_sets[CRT_SHADERS] = {
  { crt_hyllian_passes, 1 }, { crt_lottes_passes, 1 }, { crt_newpixie_passes, 4 } };

static CrtStage crt_stage[CRT_SHADERS][CRT_PASSES];
static int crt_built[CRT_SHADERS];   /* 1 built, -1 failed */
static GLuint crt_cut_prog, crt_src_tex, crt_src_fbo;
static GLint crt_cut_pos_at;
static int crt_src_w, crt_src_h, crt_flip;
static unsigned int crt_frames;
static GLenum crt_inner_internal = GL_RGBA16F, crt_inner_type = GL_HALF_FLOAT;

/* forget every GL name without deleting it: the context they belonged to is
   gone (or about to be recreated), and a new context hands out the same
   numbers for the renderer's own objects */
static void crt_reset_state(void)
{
  memset(crt_stage, 0, sizeof(crt_stage));
  memset(crt_built, 0, sizeof(crt_built));
  crt_cut_prog = 0;
  crt_src_tex = 0;
  crt_src_fbo = 0;
  crt_cut_pos_at = -1;
  crt_src_w = 0;
  crt_src_h = 0;
  crt_flip = 0;
  crt_frames = 0;
  crt_inner_internal = GL_RGBA16F;
  crt_inner_type = GL_HALF_FLOAT;
}

static void crt_free_stage(CrtStage* s)
{
  int k;
  if (s->prog) glDeleteProgram(s->prog);
  for (k = 0; k < 2; k++) {
    if (s->tex[k]) glDeleteTextures(1, &s->tex[k]);
    if (s->fbo[k]) glDeleteFramebuffers(1, &s->fbo[k]);
  }
  memset(s, 0, sizeof(*s));
}

void YglCrtDeInit(void)
{
  int i, j;
  for (i = 0; i < CRT_SHADERS; i++)
    for (j = 0; j < CRT_PASSES; j++)
      crt_free_stage(&crt_stage[i][j]);
  if (crt_cut_prog) glDeleteProgram(crt_cut_prog);
  if (crt_src_tex) glDeleteTextures(1, &crt_src_tex);
  if (crt_src_fbo) glDeleteFramebuffers(1, &crt_src_fbo);
  crt_reset_state();
}

void YglCrtForget(void)
{
  crt_reset_state();
}

int YglCrtUnavailable(int filter)
{
  const int shader = filter - AA_CRT_HYLLIAN;
  return shader >= 0 && shader < CRT_SHADERS && crt_built[shader] < 0;
}

static GLuint crt_shader(GLenum kind, const char* body)
{
  const GLchar* src[2] = { crt_version, body };
  GLint ok = GL_FALSE;
  GLuint s = glCreateShader(kind);
  glShaderSource(s, 2, src, NULL);
  glCompileShader(s);
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE) {
    char log[1024];
    glGetShaderInfoLog(s, sizeof(log), NULL, log);
    printf("CRT: shader compile error: %s\n", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static GLuint crt_program(const char* vs_body, const char* fs_body)
{
  GLint ok = GL_FALSE;
  GLuint vs = crt_shader(GL_VERTEX_SHADER, vs_body);
  GLuint fs = crt_shader(GL_FRAGMENT_SHADER, fs_body);
  GLuint p;
  if (!vs || !fs) {
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return 0;
  }
  p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glLinkProgram(p);
  glDeleteShader(vs);
  glDeleteShader(fs);
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (ok != GL_TRUE) {
    char log[1024];
    glGetProgramInfoLog(p, sizeof(log), NULL, log);
    printf("CRT: program link error: %s\n", log);
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

static int crt_build(int shader)
{
  int i;
  if (crt_built[shader]) return crt_built[shader] > 0;
  crt_built[shader] = -1;
  if (!crt_cut_prog) {
    if (!(crt_cut_prog = crt_program(crt_cut_vs, crt_cut_fs))) return 0;
    crt_cut_pos_at = glGetAttribLocation(crt_cut_prog, "a_pos");
  }
  for (i = 0; i < crt_sets[shader].n; i++) {
    CrtStage* s = &crt_stage[shader][i];
    s->pass = &crt_sets[shader].passes[i];
    if (!(s->prog = crt_program(s->pass->vs, s->pass->fs))) {
      printf("CRT: shader %d, pass %d, did not build; the filter is not used\n", shader, i);
      /* free the passes that did build */
      for (i = 0; i < crt_sets[shader].n; i++) crt_free_stage(&crt_stage[shader][i]);
      return 0;
    }
    s->pos_at = glGetAttribLocation(s->prog, s->pass->slang ? "Position" : "VertexCoord");
    s->tex_at = glGetAttribLocation(s->prog, "TexCoord");
    /* the GLSL passes read a vertex colour (Hyllian "Color", Lottes "COLOR") */
    s->col_at = glGetAttribLocation(s->prog, "Color");
    if (s->col_at < 0) s->col_at = glGetAttribLocation(s->prog, "COLOR");
  }
  crt_built[shader] = 1;
  return 1;
}

static GLuint crt_texture(GLenum internal, GLenum type, int w, int h)
{
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA, type, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return t;
}

/* (re)make a render target; 0 when it is not complete */
static int crt_target(GLuint* tex, GLuint* fbo, GLenum internal, GLenum type, int w, int h)
{
  if (*tex) glDeleteTextures(1, tex);
  if (*fbo) glDeleteFramebuffers(1, fbo);
  *tex = crt_texture(internal, type, w, h);
  glGenFramebuffers(1, fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 0;
  /* a new target starts black: NewPixie reads last frame's output
     (PassFeedback1) before anything was drawn into it */
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  return 1;
}

/* NewPixie's intermediate targets: half float like RetroArch's, or RGBA8
   where the half float format cannot be rendered to (OpenGL ES without
   EXT_color_buffer_half_float / EXT_color_buffer_float) */
static void crt_inner_target(GLuint* tex, GLuint* fbo, int w, int h)
{
  if (!crt_target(tex, fbo, crt_inner_internal, crt_inner_type, w, h) && crt_inner_internal != GL_RGBA8) {
    printf("CRT: RGBA16F is not renderable, using RGBA8\n");
    crt_inner_internal = GL_RGBA8;
    crt_inner_type = GL_UNSIGNED_BYTE;
    crt_target(tex, fbo, crt_inner_internal, crt_inner_type, w, h);
  }
}

static void crt_uniform_size(const CrtStage* s, const char* name, int w, int h)
{
  const GLint at = glGetUniformLocation(s->prog, name);
  if (at < 0) return;
  if (s->pass->slang) glUniform4f(at, (float)w, (float)h, 1.0f / w, 1.0f / h);
  else glUniform2f(at, (float)w, (float)h);
}

static void crt_bind(GLuint prog, const char* name, int unit, GLuint tex, int linear)
{
  const GLint at = glGetUniformLocation(prog, name);
  if (at < 0) return;
  glActiveTexture(GL_TEXTURE0 + unit);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
  glUniform1i(at, unit);
}

int YglCrtPresent(int filter, unsigned int src_tex, int src_w, int src_h, int field, int rotate,
                  unsigned int target_fbo, int win_x, int win_y, int win_w, int win_h,
                  int top)
{
  /* two triangles over the target with RetroArch's texture coordinates:
     x, y, 0, 1, s, t. Into a texture the first row (t = 0) is the input's
     first; into the window it is the top one. */
  static const GLfloat quad_inner[] = { -1, -1, 0, 1, 0, 0,   1, -1, 0, 1, 1, 0,
                                        -1,  1, 0, 1, 0, 1,   1,  1, 0, 1, 1, 1 };
  static const GLfloat quad_last[]  = { -1,  1, 0, 1, 0, 0,   1,  1, 0, 1, 1, 0,
                                        -1, -1, 0, 1, 0, 1,   1, -1, 0, 1, 1, 1 };
  /* rotated 90 degrees like YglBlitFramebuffer's rotate_screen: the
     picture's top line on the left of the window, its left edge at the
     bottom */
  static const GLfloat quad_last_rot[] = { -1, -1, 0, 1, 0, 0,  -1,  1, 0, 1, 1, 0,
                                            1, -1, 0, 1, 0, 1,   1,  1, 0, 1, 1, 1 };
  static const GLfloat tri_full[] = { -1, -1, 3, -1, -1, 3 };
  static const GLfloat identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
  const int shader = filter - AA_CRT_HYLLIAN;
  CrtLayout l;
  int interlaced, n, now, last, i, k, in_w, in_h;
  GLuint in;
  GLint cut[4];

  if (shader < 0 || shader >= CRT_SHADERS || src_w <= 0 || src_h <= 0 || win_w <= 0 || win_h <= 0)
    return -1;
  if (!crt_build(shader)) return -1;

  /* rotated, the picture's lines run across the window's width: lay it out
     in the picture's own orientation (window width and height swapped) */
  if (rotate) CrtLayoutCompute(src_h, win_h, win_w, field, &l);
  else CrtLayoutCompute(src_h, win_w, win_h, field, &l);
  if (top) {
    if (rotate) CrtLayoutAlignTop(&l, win_h, win_w, 1);
    else CrtLayoutAlignTop(&l, win_w, win_h, 0);
  }
  interlaced = src_h > l.lines;

  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  /* the NULL glTexImage2D allocations below must not read from a PBO */
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  /* client arrays the renderer left enabled must not be fetched by the
     passes (their unused colour inputs) */
  for (k = 0; k < 4; k++) glDisableVertexAttribArray(k);

  /* 1. the frame's (or this field's) lines, top line first */
  if (crt_src_w != src_w || crt_src_h != l.lines || !crt_src_tex) {
    crt_target(&crt_src_tex, &crt_src_fbo, GL_RGBA8, GL_UNSIGNED_BYTE, src_w, l.lines);
    crt_src_w = src_w;
    crt_src_h = l.lines;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, crt_src_fbo);
  glViewport(0, 0, src_w, l.lines);
  glUseProgram(crt_cut_prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src_tex);
  glUniform1i(glGetUniformLocation(crt_cut_prog, "u_img"), 0);
  cut[0] = src_h - 1;
  cut[1] = interlaced ? 2 : 1;
  cut[2] = interlaced ? (field & 1) : 0;
  cut[3] = 0;
  glUniform4iv(glGetUniformLocation(crt_cut_prog, "u_cut"), 1, cut);
  glEnableVertexAttribArray(crt_cut_pos_at);
  glVertexAttribPointer(crt_cut_pos_at, 2, GL_FLOAT, GL_FALSE, 0, tri_full);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glDisableVertexAttribArray(crt_cut_pos_at);

  /* 2. the passes */
  n = crt_sets[shader].n;
  now = crt_flip;
  last = crt_flip ^ 1;
  in = crt_src_tex;
  in_w = src_w;
  in_h = l.lines;
  for (i = 0; i < n; i++) {
    CrtStage* s = &crt_stage[shader][i];
    const GLfloat* quad = (i == n - 1) ? (rotate ? quad_last_rot : quad_last) : quad_inner;
    int out_w = l.w, out_h = l.h;
    if (i < n - 1) {
      if (s->pass->scale_source) { out_w = in_w; out_h = in_h; }
      if (s->w != out_w || s->h != out_h || !s->tex[0]) {
        for (k = 0; k < 2; k++) crt_inner_target(&s->tex[k], &s->fbo[k], out_w, out_h);
        s->w = out_w;
        s->h = out_h;
      }
      glBindFramebuffer(GL_FRAMEBUFFER, s->fbo[now]);
      glViewport(0, 0, out_w, out_h);
    } else {
      glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
      glViewport(win_x, win_y, win_w, win_h);
      glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      /* the odd field goes half a line towards the picture's bottom: down,
         or to the right when the top line is on the left */
      if (rotate) glViewport(win_x + l.y + l.field_shift, win_y + l.x, l.h, l.w);
      else glViewport(win_x + l.x, win_y + l.y - l.field_shift, l.w, l.h);
    }
    glUseProgram(s->prog);
    for (k = 0; k < s->pass->nparams; k++)
      glUniform1f(glGetUniformLocation(s->prog, s->pass->params[k].name), s->pass->params[k].value);
    if (s->pass->slang) {
      glUniformMatrix4fv(glGetUniformLocation(s->prog, "MVP"), 1, GL_FALSE, identity);
      crt_uniform_size(s, "SourceSize", in_w, in_h);
      crt_uniform_size(s, "OriginalSize", src_w, l.lines);
      glUniform1f(glGetUniformLocation(s->prog, "FrameCount"), (float)crt_frames);
    } else {
      glUniformMatrix4fv(glGetUniformLocation(s->prog, "MVPMatrix"), 1, GL_FALSE, identity);
      crt_uniform_size(s, "TextureSize", in_w, in_h);
      crt_uniform_size(s, "InputSize", in_w, in_h);
      glUniform1i(glGetUniformLocation(s->prog, "FrameDirection"), 1);
      glUniform1i(glGetUniformLocation(s->prog, "FrameCount"), (GLint)crt_frames);
    }
    crt_uniform_size(s, "OutputSize", out_w, out_h);
    /* the input on unit 0 whatever the shader calls it (Hyllian's is s_p) */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, in);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s->pass->linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s->pass->linear ? GL_LINEAR : GL_NEAREST);
    glUniform1i(glGetUniformLocation(s->prog, s->pass->slang ? "Source" : "Texture"), 0);
    glUniform1i(glGetUniformLocation(s->prog, "s_p"), 0);
    if (n == CRT_PASSES) {
      /* NewPixie: the last frame's second pass, this frame's first and third */
      crt_bind(s->prog, "PassFeedback1", 1, crt_stage[shader][1].tex[last], 1);
      crt_bind(s->prog, "accum1", 2, crt_stage[shader][0].tex[now], 1);
      crt_bind(s->prog, "blur2", 3, crt_stage[shader][2].tex[now], 1);
    }
    if (s->pos_at >= 0) {
      glEnableVertexAttribArray(s->pos_at);
      glVertexAttribPointer(s->pos_at, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat), quad);
    }
    if (s->tex_at >= 0) {
      glEnableVertexAttribArray(s->tex_at);
      glVertexAttribPointer(s->tex_at, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat), quad + 4);
    }
    if (s->col_at >= 0) {
      glDisableVertexAttribArray(s->col_at);
      glVertexAttrib4f(s->col_at, 1.0f, 1.0f, 1.0f, 1.0f);
    }
    /* the window keeps the alpha of 1 it was cleared to: the shaders' own
       alpha (Hyllian's is below 1 on the scan-line edges) must not make
       the window or a screenshot of it translucent */
    if (i == n - 1) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (i == n - 1) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (s->pos_at >= 0) glDisableVertexAttribArray(s->pos_at);
    if (s->tex_at >= 0) glDisableVertexAttribArray(s->tex_at);
    if (i < n - 1) {
      in = s->tex[now];
      in_w = out_w;
      in_h = out_h;
    }
  }
  for (k = 3; k >= 0; k--) {
    glActiveTexture(GL_TEXTURE0 + k);
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
  glUseProgram(0);
  glEnable(GL_BLEND);
  crt_flip ^= 1;
  crt_frames++;
  return 0;
}
