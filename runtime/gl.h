// OpenGL 3.3 core functions, loaded through SDL (gl_load() after the context exists).
#pragma once
#include <SDL.h>
#include <SDL_opengl.h>

#define GL_FUNCS(X)                                                                                      \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource)                    \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv)                    \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram)          \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram)                      \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)          \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLDELETESHADERPROC, glDeleteShader)                        \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) X(PFNGLUNIFORM1IPROC, glUniform1i)              \
    X(PFNGLUNIFORM1FPROC, glUniform1f) X(PFNGLUNIFORM2FPROC, glUniform2f) X(PFNGLUNIFORM4FPROC, glUniform4f) \
    X(PFNGLUNIFORM1UIPROC, glUniform1ui) X(PFNGLUNIFORM1UIVPROC, glUniform1uiv)                          \
    X(PFNGLUNIFORM4UIVPROC, glUniform4uiv) X(PFNGLUNIFORM1FVPROC, glUniform1fv)                          \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)        \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer)                            \
    X(PFNGLBUFFERDATAPROC, glBufferData) X(PFNGLBUFFERSUBDATAPROC, glBufferSubData)                      \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)                                               \
    X(PFNGLVERTEXATTRIBIPOINTERPROC, glVertexAttribIPointer)                                             \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)                                       \
    X(PFNGLACTIVETEXTUREPROC, rt_glActiveTexture) X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)         \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)                                         \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)  \
    X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap) X(PFNGLTEXIMAGE3DPROC, rt_glTexImage3D)                    \
    X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase) X(PFNGLGETUNIFORMBLOCKINDEXPROC, glGetUniformBlockIndex) \
    X(PFNGLUNIFORMBLOCKBINDINGPROC, glUniformBlockBinding) X(PFNGLDRAWBUFFERSPROC, glDrawBuffers)        \
    X(PFNGLTEXBUFFERPROC, glTexBuffer) X(PFNGLBINDFRAGDATALOCATIONPROC, glBindFragDataLocation)

#define GL_DECLARE(type, name) extern type name;
GL_FUNCS(GL_DECLARE)
#undef GL_DECLARE

// SDL_opengl.h declares these two already; ours are loaded under another name.
#define glActiveTexture rt_glActiveTexture
#define glTexImage3D rt_glTexImage3D

bool gl_load();
