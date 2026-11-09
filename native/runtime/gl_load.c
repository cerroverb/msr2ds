/* OpenGL functions from the current context (gl_load.h). */
#include "gl_load.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define X(ret, name, args) msr_pfn_##name msr_##name;
MSR_GL_FUNCS(X)
#undef X

static int version;

int msr_gl_load(const char **missing) {
#define X(ret, name, args) \
    if (!(msr_##name = (msr_pfn_##name)SDL_GL_GetProcAddress(#name))) { if (missing) *missing = #name; return 0; }
    MSR_GL_FUNCS(X)
#undef X
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major); glGetIntegerv(GL_MINOR_VERSION, &minor);
    version = major * 10 + minor;
    return 1;
}

int msr_gl_version(void) { return version; }

int msr_gl_has(const char *ext) {
    GLint n = 0; glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) { const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i); if (e && !strcmp(e, ext)) return 1; }
    return 0;
}

GLuint msr_gl_shader(const char *name, GLenum type, const char *header, const char *src) {
    const char *body = strchr(src, '\n');   /* past the source's #version line */
    body = body ? body + 1 : src;
    const char *parts[3] = { header, "\n#define VARYING(n)\n#define gl_VertexIndex gl_VertexID\n#line 2\n", body };
    GLuint s = glCreateShader(type);
    if (header) glShaderSource(s, 3, parts, NULL);
    else glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = ""; glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "video: OpenGL %s %s shader: %s\n", name, type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(s); return 0;
    }
    return s;
}

GLuint msr_gl_link(const char *name, GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = ""; glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "video: OpenGL %s program: %s\n", name, log);
        glDeleteProgram(p); return 0;
    }
    return p;
}

GLuint msr_gl_program(const char *name, const char *header, const char *vs, const char *fs) {
    GLuint v = msr_gl_shader(name, GL_VERTEX_SHADER, header, vs), f = v ? msr_gl_shader(name, GL_FRAGMENT_SHADER, header, fs) : 0;
    GLuint p = f ? msr_gl_link(name, v, f) : 0;
    if (v) glDeleteShader(v);
    if (f) glDeleteShader(f);
    return p;
}
