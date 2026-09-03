// Headless GL 4.1 core context on macOS (CGL), used to compile the plugin's
// shaders exactly as GaussianRenderer::loadShader would.
#include <GL/glew.h>
#include <OpenGL/OpenGL.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static std::string slurp(const std::string& p) {
    std::ifstream f(p);
    if (!f) { fprintf(stderr, "cannot open %s\n", p.c_str()); exit(2); }
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

static GLuint compile(GLenum type, const std::string& path, const std::string& preamble) {
    std::string src = preamble + "#line 1\n" + slurp(path);
    const char* p = src.c_str();
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    GLint len = 0; glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
    if (len > 1) { std::string log(len, '\0'); glGetShaderInfoLog(s, len, nullptr, &log[0]);
                   printf("  log for %s:\n%s\n", path.c_str(), log.c_str()); }
    printf("  compile %-40s : %s\n", path.c_str(), ok ? "OK" : "FAILED");
    return ok ? s : 0;
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "src/shaders/";

    CGLPixelFormatAttribute attribs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
        kCGLPFAAccelerated,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)0
    };
    CGLPixelFormatObj pix = nullptr; GLint npix = 0;
    if (CGLChoosePixelFormat(attribs, &pix, &npix) != kCGLNoError || !pix) {
        fprintf(stderr, "CGLChoosePixelFormat failed\n"); return 2;
    }
    CGLContextObj ctx = nullptr;
    if (CGLCreateContext(pix, nullptr, &ctx) != kCGLNoError) {
        fprintf(stderr, "CGLCreateContext failed\n"); return 2;
    }
    CGLSetCurrentContext(ctx);

    glewExperimental = GL_TRUE;
    glewInit();
    while (glGetError() != GL_NO_ERROR) {} // GLEW core-profile probing noise

    printf("GL_VERSION  : %s\n", (const char*)glGetString(GL_VERSION));
    printf("GLSL        : %s\n", (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
    printf("compute     : %s\n", GLEW_ARB_compute_shader ? "yes" : "no");
    printf("SSBO        : %s\n", GLEW_ARB_shader_storage_buffer_object ? "yes" : "no");
    GLint maxTB = 0; glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &maxTB);
    printf("max TBO texels: %d (%.1fM)\n\n", maxTB, maxTB / 1e6);

    printf("Building GL 4.1 draw program:\n");
    GLuint v = compile(GL_VERTEX_SHADER,   dir + "gaussian.vert", "#version 410 core\n");
    GLuint f = compile(GL_FRAGMENT_SHADER, dir + "gaussian.frag", "#version 410 core\n");
    if (!v || !f) return 1;

    GLuint prog = glCreateProgram();
    glAttachShader(prog, v); glAttachShader(prog, f);
    glLinkProgram(prog);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    GLint len = 0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
    if (len > 1) { std::string log(len, '\0'); glGetProgramInfoLog(prog, len, nullptr, &log[0]);
                   printf("  link log:\n%s\n", log.c_str()); }
    printf("  link                                     : %s\n\n", ok ? "OK" : "FAILED");
    if (!ok) return 1;

    // Every uniform the renderer looks up must actually resolve, or the 4.1
    // path would silently bind nothing.
    const char* uniforms[] = {
        "u_wvm","u_pm","u_splatScale","u_opacityMult","u_viewport","u_shDegree",
        "u_restFloatsPerSplat","u_sRGBToLinear","u_gamma","u_camPos",
        "u_posTex","u_rotTex","u_sclTex","u_shTex","u_shRestTex"
    };
    int missing = 0;
    printf("Uniform locations:\n");
    for (const char* u : uniforms) {
        GLint loc = glGetUniformLocation(prog, u);
        printf("  %-22s %d%s\n", u, loc, loc < 0 ? "   <-- MISSING" : "");
        if (loc < 0) ++missing;
    }
    GLint attr = glGetAttribLocation(prog, "a_splatIndex");
    printf("\nAttribute a_splatIndex location: %d%s\n", attr, attr < 0 ? "   <-- MISSING" : "");
    if (attr < 0) ++missing;

    printf("\n%s\n", missing ? "RESULT: FAILED" : "RESULT: PASS");
    return missing ? 1 : 0;
}
