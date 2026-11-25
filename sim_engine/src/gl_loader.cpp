#include "gl_loader.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define NSIM_DEF_GL(type, name) type name = nullptr;
NSIM_GL_FUNCS(NSIM_DEF_GL)
#undef NSIM_DEF_GL

namespace nsim {

namespace {

using ProcFn = void *(*)(const char *);

void *load_proc(const char *name) {
#ifdef _WIN32
    void *p = reinterpret_cast<void *>(wglGetProcAddress(name));
    if (!p) {
        static HMODULE gl_mod = LoadLibraryA("opengl32.dll");
        if (gl_mod) p = reinterpret_cast<void *>(GetProcAddress(gl_mod, name));
    }
    return p;
#else
    static void *gl_lib = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    static ProcFn glx_get = gl_lib ? reinterpret_cast<ProcFn>(dlsym(gl_lib, "glXGetProcAddressARB")) : nullptr;
    if (glx_get) {
        if (void *p = glx_get(name)) return p;
    }
    return gl_lib ? dlsym(gl_lib, name) : nullptr;
#endif
}

std::string read_text_file(const std::string &path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        std::fprintf(stderr, "[GL] Failed to open shader: %s\n", path.c_str());
        return {};
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

GLuint compile_stage(GLenum stage, const std::string &src, const std::string &label) {
    GLuint sh = glCreateShader(stage);
    const char *ptr = src.c_str();
    glShaderSource(sh, 1, &ptr, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::string log(std::max(1, len), '\0');
        glGetShaderInfoLog(sh, len, nullptr, log.data());
        std::fprintf(stderr, "[GL] Shader compile error (%s):\n%s\n", label.c_str(), log.c_str());
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

} // namespace

bool load_gl_functions() {
    bool all_ok = true;
#define NSIM_LOAD_GL(type, name) \
    name = reinterpret_cast<type>(load_proc(#name)); \
    if (!name) { std::fprintf(stderr, "[GL] Missing symbol: %s\n", #name); all_ok = false; }
    NSIM_GL_FUNCS(NSIM_LOAD_GL)
#undef NSIM_LOAD_GL
    return all_ok;
}

GLuint compile_program_files(const std::string &vert_path, const std::string &frag_path) {
    std::string vsrc = read_text_file(vert_path);
    std::string fsrc = read_text_file(frag_path);
    if (vsrc.empty() || fsrc.empty()) return 0;
    GLuint vs = compile_stage(GL_VERTEX_SHADER, vsrc, vert_path);
    GLuint fs = compile_stage(GL_FRAGMENT_SHADER, fsrc, frag_path);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(std::max(1, len), '\0');
        glGetProgramInfoLog(prog, len, nullptr, log.data());
        std::fprintf(stderr, "[GL] Program link error (%s + %s):\n%s\n", vert_path.c_str(), frag_path.c_str(), log.c_str());
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

GLuint compile_compute_file(const std::string &comp_path) {
    std::string csrc = read_text_file(comp_path);
    if (csrc.empty()) return 0;
    GLuint cs = compile_stage(GL_COMPUTE_SHADER, csrc, comp_path);
    if (!cs) return 0;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, cs);
    glLinkProgram(prog);
    glDeleteShader(cs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(std::max(1, len), '\0');
        glGetProgramInfoLog(prog, len, nullptr, log.data());
        std::fprintf(stderr, "[GL] Compute link error (%s):\n%s\n", comp_path.c_str(), log.c_str());
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

} // namespace nsim
