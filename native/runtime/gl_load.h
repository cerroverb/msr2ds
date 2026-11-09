/* OpenGL 3.3 core functions, from the context through SDL_GL_GetProcAddress (gl_load.c): no OpenGL library is linked, so
 * the program starts where there is none. Types and constants come from SDL's headers. Include this, not SDL_opengl.h. */
#ifndef PWR_GL_LOAD_H
#define PWR_GL_LOAD_H
#include <SDL_opengl.h>

/* X(return type, name, (parameters)) */
#define MSR_GL_FUNCS(X) \
    X(const GLubyte *, glGetString, (GLenum name)) \
    X(const GLubyte *, glGetStringi, (GLenum name, GLuint index)) \
    X(void, glGetIntegerv, (GLenum pname, GLint *data)) \
    X(GLenum, glGetError, (void)) \
    X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h)) \
    X(void, glScissor, (GLint x, GLint y, GLsizei w, GLsizei h)) \
    X(void, glEnable, (GLenum cap)) \
    X(void, glDisable, (GLenum cap)) \
    X(void, glBlendFunc, (GLenum s, GLenum d)) \
    X(void, glBlendFuncSeparate, (GLenum sc, GLenum dc, GLenum sa, GLenum da)) \
    X(void, glBlendEquationSeparate, (GLenum c, GLenum a)) \
    X(void, glBlendColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
    X(void, glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a)) \
    X(void, glDepthMask, (GLboolean flag)) \
    X(void, glDepthFunc, (GLenum func)) \
    X(void, glStencilFunc, (GLenum func, GLint ref, GLuint mask)) \
    X(void, glStencilOp, (GLenum sfail, GLenum dpfail, GLenum dppass)) \
    X(void, glStencilMask, (GLuint mask)) \
    X(void, glCullFace, (GLenum mode)) \
    X(void, glFrontFace, (GLenum mode)) \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
    X(void, glClearDepth, (GLdouble d)) \
    X(void, glClearStencil, (GLint s)) \
    X(void, glClear, (GLbitfield mask)) \
    X(void, glClearBufferfv, (GLenum buffer, GLint drawbuffer, const GLfloat *value)) \
    X(void, glClearBufferfi, (GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil)) \
    X(void, glFinish, (void)) \
    X(void, glFlush, (void)) \
    X(void, glPixelStorei, (GLenum pname, GLint param)) \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *pixels)) \
    X(void, glReadBuffer, (GLenum src)) \
    X(void, glDrawBuffer, (GLenum buf)) \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count)) \
    X(void, glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void *indices)) \
    X(void, glDrawElementsBaseVertex, (GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex)) \
    /* textures */ \
    X(void, glGenTextures, (GLsizei n, GLuint *textures)) \
    X(void, glDeleteTextures, (GLsizei n, const GLuint *textures)) \
    X(void, glBindTexture, (GLenum target, GLuint texture)) \
    X(void, glActiveTexture, (GLenum texture)) \
    X(void, glTexImage2D, (GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void *pixels)) \
    X(void, glTexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels)) \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param)) \
    X(void, glTexParameterf, (GLenum target, GLenum pname, GLfloat param)) \
    X(void, glTexParameterfv, (GLenum target, GLenum pname, const GLfloat *params)) \
    X(void, glTexBuffer, (GLenum target, GLenum internalformat, GLuint buffer)) \
    X(void, glGenerateMipmap, (GLenum target)) \
    X(void, glCopyTexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLint sx, GLint sy, GLsizei w, GLsizei h)) \
    X(void, glGenSamplers, (GLsizei n, GLuint *samplers)) \
    X(void, glDeleteSamplers, (GLsizei n, const GLuint *samplers)) \
    X(void, glBindSampler, (GLuint unit, GLuint sampler)) \
    X(void, glSamplerParameteri, (GLuint sampler, GLenum pname, GLint param)) \
    X(void, glSamplerParameterf, (GLuint sampler, GLenum pname, GLfloat param)) \
    /* framebuffers */ \
    X(void, glGenFramebuffers, (GLsizei n, GLuint *framebuffers)) \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint *framebuffers)) \
    X(void, glBindFramebuffer, (GLenum target, GLuint framebuffer)) \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum target)) \
    X(void, glBlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)) \
    X(void, glDrawBuffers, (GLsizei n, const GLenum *bufs)) \
    /* buffers, vertex arrays */ \
    X(void, glGenBuffers, (GLsizei n, GLuint *buffers)) \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint *buffers)) \
    X(void, glBindBuffer, (GLenum target, GLuint buffer)) \
    X(void, glBindBufferBase, (GLenum target, GLuint index, GLuint buffer)) \
    X(void, glBindBufferRange, (GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size)) \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage)) \
    X(void, glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void *data)) \
    X(void *, glMapBufferRange, (GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)) \
    X(GLboolean, glUnmapBuffer, (GLenum target)) \
    X(void, glGenVertexArrays, (GLsizei n, GLuint *arrays)) \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint *arrays)) \
    X(void, glBindVertexArray, (GLuint array)) \
    X(void, glEnableVertexAttribArray, (GLuint index)) \
    X(void, glDisableVertexAttribArray, (GLuint index)) \
    X(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)) \
    X(void, glVertexAttribIPointer, (GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer)) \
    /* shaders */ \
    X(GLuint, glCreateShader, (GLenum type)) \
    X(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)) \
    X(void, glCompileShader, (GLuint shader)) \
    X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint *params)) \
    X(void, glGetShaderInfoLog, (GLuint shader, GLsizei size, GLsizei *length, GLchar *log)) \
    X(void, glDeleteShader, (GLuint shader)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint program, GLuint shader)) \
    X(void, glBindAttribLocation, (GLuint program, GLuint index, const GLchar *name)) \
    X(void, glLinkProgram, (GLuint program)) \
    X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint *params)) \
    X(void, glGetProgramInfoLog, (GLuint program, GLsizei size, GLsizei *length, GLchar *log)) \
    X(void, glDeleteProgram, (GLuint program)) \
    X(void, glUseProgram, (GLuint program)) \
    X(GLint, glGetUniformLocation, (GLuint program, const GLchar *name)) \
    X(GLuint, glGetUniformBlockIndex, (GLuint program, const GLchar *name)) \
    X(void, glUniformBlockBinding, (GLuint program, GLuint index, GLuint binding)) \
    X(void, glUniform1i, (GLint location, GLint v)) \
    X(void, glUniform4fv, (GLint location, GLsizei count, const GLfloat *v)) \
    /* sync */ \
    X(GLsync, glFenceSync, (GLenum condition, GLbitfield flags)) \
    X(GLenum, glClientWaitSync, (GLsync sync, GLbitfield flags, GLuint64 timeout)) \
    X(void, glWaitSync, (GLsync sync, GLbitfield flags, GLuint64 timeout)) \
    X(void, glDeleteSync, (GLsync sync)) \
    /* timer queries (core 3.3) */ \
    X(void, glGenQueries, (GLsizei n, GLuint *ids)) \
    X(void, glDeleteQueries, (GLsizei n, const GLuint *ids)) \
    X(void, glBeginQuery, (GLenum target, GLuint id)) \
    X(void, glEndQuery, (GLenum target)) \
    X(void, glGetQueryObjectiv, (GLuint id, GLenum pname, GLint *params)) \
    X(void, glGetQueryObjectui64v, (GLuint id, GLenum pname, GLuint64 *params))

#define X(ret, name, args) typedef ret (APIENTRY *msr_pfn_##name) args; extern msr_pfn_##name msr_##name;
MSR_GL_FUNCS(X)
#undef X
/* the calls below go through the table */
#define glGetString msr_glGetString
#define glGetStringi msr_glGetStringi
#define glGetIntegerv msr_glGetIntegerv
#define glGetError msr_glGetError
#define glViewport msr_glViewport
#define glScissor msr_glScissor
#define glEnable msr_glEnable
#define glDisable msr_glDisable
#define glBlendFunc msr_glBlendFunc
#define glBlendFuncSeparate msr_glBlendFuncSeparate
#define glBlendEquationSeparate msr_glBlendEquationSeparate
#define glBlendColor msr_glBlendColor
#define glColorMask msr_glColorMask
#define glDepthMask msr_glDepthMask
#define glDepthFunc msr_glDepthFunc
#define glStencilFunc msr_glStencilFunc
#define glStencilOp msr_glStencilOp
#define glStencilMask msr_glStencilMask
#define glCullFace msr_glCullFace
#define glFrontFace msr_glFrontFace
#define glClearColor msr_glClearColor
#define glClearDepth msr_glClearDepth
#define glClearStencil msr_glClearStencil
#define glClear msr_glClear
#define glClearBufferfv msr_glClearBufferfv
#define glClearBufferfi msr_glClearBufferfi
#define glFinish msr_glFinish
#define glFlush msr_glFlush
#define glPixelStorei msr_glPixelStorei
#define glReadPixels msr_glReadPixels
#define glReadBuffer msr_glReadBuffer
#define glDrawBuffer msr_glDrawBuffer
#define glDrawArrays msr_glDrawArrays
#define glDrawElements msr_glDrawElements
#define glDrawElementsBaseVertex msr_glDrawElementsBaseVertex
#define glGenTextures msr_glGenTextures
#define glDeleteTextures msr_glDeleteTextures
#define glBindTexture msr_glBindTexture
#define glActiveTexture msr_glActiveTexture
#define glTexImage2D msr_glTexImage2D
#define glTexSubImage2D msr_glTexSubImage2D
#define glTexParameteri msr_glTexParameteri
#define glTexParameterf msr_glTexParameterf
#define glTexParameterfv msr_glTexParameterfv
#define glTexBuffer msr_glTexBuffer
#define glGenerateMipmap msr_glGenerateMipmap
#define glCopyTexSubImage2D msr_glCopyTexSubImage2D
#define glGenSamplers msr_glGenSamplers
#define glDeleteSamplers msr_glDeleteSamplers
#define glBindSampler msr_glBindSampler
#define glSamplerParameteri msr_glSamplerParameteri
#define glSamplerParameterf msr_glSamplerParameterf
#define glGenFramebuffers msr_glGenFramebuffers
#define glDeleteFramebuffers msr_glDeleteFramebuffers
#define glBindFramebuffer msr_glBindFramebuffer
#define glFramebufferTexture2D msr_glFramebufferTexture2D
#define glCheckFramebufferStatus msr_glCheckFramebufferStatus
#define glBlitFramebuffer msr_glBlitFramebuffer
#define glDrawBuffers msr_glDrawBuffers
#define glGenBuffers msr_glGenBuffers
#define glDeleteBuffers msr_glDeleteBuffers
#define glBindBuffer msr_glBindBuffer
#define glBindBufferBase msr_glBindBufferBase
#define glBindBufferRange msr_glBindBufferRange
#define glBufferData msr_glBufferData
#define glBufferSubData msr_glBufferSubData
#define glMapBufferRange msr_glMapBufferRange
#define glUnmapBuffer msr_glUnmapBuffer
#define glGenVertexArrays msr_glGenVertexArrays
#define glDeleteVertexArrays msr_glDeleteVertexArrays
#define glBindVertexArray msr_glBindVertexArray
#define glEnableVertexAttribArray msr_glEnableVertexAttribArray
#define glDisableVertexAttribArray msr_glDisableVertexAttribArray
#define glVertexAttribPointer msr_glVertexAttribPointer
#define glVertexAttribIPointer msr_glVertexAttribIPointer
#define glCreateShader msr_glCreateShader
#define glShaderSource msr_glShaderSource
#define glCompileShader msr_glCompileShader
#define glGetShaderiv msr_glGetShaderiv
#define glGetShaderInfoLog msr_glGetShaderInfoLog
#define glDeleteShader msr_glDeleteShader
#define glCreateProgram msr_glCreateProgram
#define glAttachShader msr_glAttachShader
#define glBindAttribLocation msr_glBindAttribLocation
#define glLinkProgram msr_glLinkProgram
#define glGetProgramiv msr_glGetProgramiv
#define glGetProgramInfoLog msr_glGetProgramInfoLog
#define glDeleteProgram msr_glDeleteProgram
#define glUseProgram msr_glUseProgram
#define glGetUniformLocation msr_glGetUniformLocation
#define glGetUniformBlockIndex msr_glGetUniformBlockIndex
#define glUniformBlockBinding msr_glUniformBlockBinding
#define glUniform1i msr_glUniform1i
#define glUniform4fv msr_glUniform4fv
#define glFenceSync msr_glFenceSync
#define glClientWaitSync msr_glClientWaitSync
#define glWaitSync msr_glWaitSync
#define glDeleteSync msr_glDeleteSync
#define glGenQueries msr_glGenQueries
#define glDeleteQueries msr_glDeleteQueries
#define glBeginQuery msr_glBeginQuery
#define glEndQuery msr_glEndQuery
#define glGetQueryObjectiv msr_glGetQueryObjectiv
#define glGetQueryObjectui64v msr_glGetQueryObjectui64v

/* every function from the current context; 0 when one is missing (its name in *missing) */
int msr_gl_load(const char **missing);
/* the context's version (e.g. 33, 46) and whether it has an extension */
int msr_gl_version(void);
int msr_gl_has(const char *ext);
/* a program from vertex and fragment GLSL written for Vulkan (shaders/): their #version line is replaced by `header`
   (e.g. "#version 330 core"), VARYING(n) is defined empty (GLSL 3.30 matches varyings by name) and gl_VertexIndex is
   gl_VertexID. 0 on failure, logged with `name`. */
GLuint msr_gl_program(const char *name, const char *header, const char *vs, const char *fs);
/* the same in steps: one shader (header NULL: src as it is, its own #version line kept), and a program of two */
GLuint msr_gl_shader(const char *name, GLenum type, const char *header, const char *src);
GLuint msr_gl_link(const char *name, GLuint vs, GLuint fs);
#endif
