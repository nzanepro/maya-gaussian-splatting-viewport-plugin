#include "GaussianRenderer.h"
#include <maya/MGlobal.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <numeric>
#include <algorithm>
#include <mutex>
#include <cstring>
#include <cstdint>
#include <limits>

std::string GaussianRenderer::s_shaderDir;

GaussianRenderer::GaussianRenderer() {}

GaussianRenderer::~GaussianRenderer() {
    destroyGL();
}

namespace {

// Map an IEEE-754 float onto a uint32 whose unsigned ordering matches the
// float's ordering: flip every bit of a negative, flip only the sign bit of a
// positive. Lets the radix sort below work on raw depth values.
inline uint32_t depthToRadixKey(float z) {
    uint32_t bits;
    std::memcpy(&bits, &z, sizeof(bits));
    return (bits & 0x80000000u) ? ~bits : (bits | 0x80000000u);
}

// LSD radix sort, 4 passes of 8 bits, sorting `idx` by `keys` ascending.
// std::sort would cost ~4x this on the million-splat scenes the plugin
// targets, and this runs every frame the camera moves.
void radixSortByKey(std::vector<uint32_t>& keys, std::vector<uint32_t>& idx,
                    std::vector<uint32_t>& keysTmp, std::vector<uint32_t>& idxTmp) {
    const size_t n = keys.size();
    if (n < 2) return;
    keysTmp.resize(n);
    idxTmp.resize(n);

    uint32_t hist[4][256] = {};
    for (size_t i = 0; i < n; ++i) {
        const uint32_t k = keys[i];
        ++hist[0][ k        & 0xFFu];
        ++hist[1][(k >>  8) & 0xFFu];
        ++hist[2][(k >> 16) & 0xFFu];
        ++hist[3][(k >> 24) & 0xFFu];
    }

    for (int pass = 0; pass < 4; ++pass) {
        const int shift = pass * 8;
        // Every key shares this digit — the pass would be an exact copy.
        // Common for the high byte, where all depths have the same exponent.
        if (hist[pass][(keys[0] >> shift) & 0xFFu] == n) continue;

        uint32_t offset[256];
        uint32_t sum = 0;
        for (int b = 0; b < 256; ++b) { offset[b] = sum; sum += hist[pass][b]; }

        for (size_t i = 0; i < n; ++i) {
            const uint32_t d = offset[(keys[i] >> shift) & 0xFFu]++;
            keysTmp[d] = keys[i];
            idxTmp[d]  = idx[i];
        }
        // Swapping after every scatter keeps the live data in keys/idx, so
        // skipped passes cannot leave the result in the scratch vectors.
        keys.swap(keysTmp);
        idx.swap(idxTmp);
    }
}

} // namespace

void GaussianRenderer::detectCapabilities() {
    if (capsChecked_) return;
    capsChecked_ = true;

    useCompute_ = (GLEW_ARB_compute_shader && GLEW_ARB_shader_storage_buffer_object);
    dataTarget_ = useCompute_ ? GL_SHADER_STORAGE_BUFFER : GL_TEXTURE_BUFFER;

    if (useCompute_) {
        MGlobal::displayInfo("[GaussianSplat] Render path: OpenGL 4.3 (compute sort + SSBO).");
    } else {
        MGlobal::displayInfo(
            "[GaussianSplat] Render path: OpenGL 4.1 (CPU sort + texture buffers). "
            "Compute shaders and SSBOs are unavailable on this context.");
    }
}

void GaussianRenderer::initGL() {
    if (vao_ != 0) return;

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    glGenBuffers(1, &posBuf_);
    glGenBuffers(1, &rotBuf_);
    glGenBuffers(1, &sclBuf_);
    glGenBuffers(1, &shBuf_);
    glGenBuffers(1, &shRestBuf_);
    glGenBuffers(1, &indexBuf_);
    glGenBuffers(1, &keyBuf_);

    buildShaderProgram();
    buildSortProgram();
    glBindVertexArray(0);
}

void GaussianRenderer::destroyGL() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (posBuf_) glDeleteBuffers(1, &posBuf_);
    if (rotBuf_) glDeleteBuffers(1, &rotBuf_);
    if (sclBuf_) glDeleteBuffers(1, &sclBuf_);
    if (shBuf_) glDeleteBuffers(1, &shBuf_);
    if (shRestBuf_) glDeleteBuffers(1, &shRestBuf_);
    if (indexBuf_) glDeleteBuffers(1, &indexBuf_);
    if (keyBuf_) glDeleteBuffers(1, &keyBuf_);
    if (posTex_) glDeleteTextures(1, &posTex_);
    if (rotTex_) glDeleteTextures(1, &rotTex_);
    if (sclTex_) glDeleteTextures(1, &sclTex_);
    if (shTex_) glDeleteTextures(1, &shTex_);
    if (shRestTex_) glDeleteTextures(1, &shRestTex_);
    if (drawProgram_) glDeleteProgram(drawProgram_);
    if (sortProgram_) glDeleteProgram(sortProgram_);
    if (depthProgram_) glDeleteProgram(depthProgram_);
    vao_ = 0;
}

// GL 4.1 path: wrap each data buffer in a texture buffer object so the vertex
// shader can texelFetch it. The buffer objects themselves are shared with the
// compute path; only the view differs.
void GaussianRenderer::createTextureBuffers() {
    if (posTex_ == 0) {
        glGenTextures(1, &posTex_);
        glGenTextures(1, &rotTex_);
        glGenTextures(1, &sclTex_);
        glGenTextures(1, &shTex_);
        glGenTextures(1, &shRestTex_);
    }

    const struct { GLuint tex; GLuint buf; GLenum fmt; } views[] = {
        { posTex_,    posBuf_,    GL_RGBA32F },
        { rotTex_,    rotBuf_,    GL_RGBA32F },
        { sclTex_,    sclBuf_,    GL_RGBA32F },
        { shTex_,     shBuf_,     GL_RGBA32F },
        { shRestTex_, shRestBuf_, GL_R32F    }, // one float per texel
    };
    for (const auto& v : views) {
        glBindTexture(GL_TEXTURE_BUFFER, v.tex);
        glTexBuffer(GL_TEXTURE_BUFFER, v.fmt, v.buf);
    }
    glBindTexture(GL_TEXTURE_BUFFER, 0);

    // A texture buffer is capped well below the size of a shader storage
    // buffer, and sh_rest is by far the largest array (up to 45 floats per
    // splat). Warn rather than render a silently truncated scene.
    GLint maxTexels = 0;
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &maxTexels);
    const long long restTexels = (long long)splatCount_ * restFloatsPerSplat_;
    if (maxTexels > 0 && restTexels > (long long)maxTexels) {
        MGlobal::displayWarning(
            MString("[GaussianSplat] SH coefficients need ") + (int)(restTexels / 1000000)
            + "M texels but this GL implementation caps texture buffers at "
            + (int)(maxTexels / 1000000) + "M. Lower shDegree to reduce the buffer size.");
    }
}

void GaussianRenderer::uploadSplats(const SplatData& data) {
    initGL();
    splatCount_         = data.splatCount;
    loadedShDegree_     = (data.shDegree >= 1 && !data.sh_rest.empty()) ? data.shDegree : 0;
    restFloatsPerSplat_ = (loadedShDegree_ >= 1) ? data.restFloatsPerSplat : 0;
    sortDirty_          = true; // force re-sort after new data
    if (splatCount_ == 0) return;

    // Compute next power-of-2 for the bitonic sort — it must operate on n entries
    // where n >= splatCount_, otherwise out-of-bounds writes corrupt GPU memory.
    // The CPU sort has no such constraint and works on splatCount_ directly.
    sortN_ = 1;
    while (sortN_ < (uint32_t)splatCount_) sortN_ <<= 1;
    const uint32_t indexCount = useCompute_ ? sortN_ : (uint32_t)splatCount_;

    // dataTarget_ is GL_SHADER_STORAGE_BUFFER on the compute path and
    // GL_TEXTURE_BUFFER on the 4.1 path, where the SSBO enum is not a legal
    // binding point. The buffer contents are identical either way.
    glBindBuffer(dataTarget_, posBuf_);
    glBufferData(dataTarget_, data.positions.size() * sizeof(float), data.positions.data(), GL_STATIC_DRAW);

    glBindBuffer(dataTarget_, rotBuf_);
    glBufferData(dataTarget_, data.rotations.size() * sizeof(float), data.rotations.data(), GL_STATIC_DRAW);

    glBindBuffer(dataTarget_, sclBuf_);
    glBufferData(dataTarget_, data.scales.size() * sizeof(float), data.scales.data(), GL_STATIC_DRAW);

    glBindBuffer(dataTarget_, shBuf_);
    glBufferData(dataTarget_, data.sh_dc.size() * sizeof(float), data.sh_dc.data(), GL_STATIC_DRAW);

    // SH rest buffer (binding 6): restFloatsPerSplat_ floats per splat (9/24/45).
    // Upload a single zero float when SH rest is absent so the buffer object is valid.
    glBindBuffer(dataTarget_, shRestBuf_);
    if (loadedShDegree_ >= 1) {
        glBufferData(dataTarget_, data.sh_rest.size() * sizeof(float),
                     data.sh_rest.data(), GL_STATIC_DRAW);
    } else {
        float zero = 0.0f;
        glBufferData(dataTarget_, sizeof(float), &zero, GL_STATIC_DRAW);
    }
    glBindBuffer(dataTarget_, 0);

    // IndexBuffer: identity order to start with.
    // Compute path — size sortN_, padding entries [splatCount_..sortN_-1] hold
    // index 0 (a safe dummy that is never drawn).
    std::vector<uint32_t> indices(indexCount, 0);
    std::iota(indices.begin(), indices.begin() + splatCount_, 0);

    if (useCompute_) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, indexBuf_);
        glBufferData(GL_SHADER_STORAGE_BUFFER, indexCount * sizeof(uint32_t),
                     indices.data(), GL_DYNAMIC_DRAW);

        // KeyBuffer (depths): size = sortN_. Fill ALL entries with +INF so padding sorts to end.
        const float kInf = std::numeric_limits<float>::infinity();
        std::vector<float> depths(sortN_, kInf);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, keyBuf_);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sortN_ * sizeof(float), depths.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        hostPositions_.clear();
        hostPositions_.shrink_to_fit();
    } else {
        // GL 4.1 path: the sorted index is an instanced vertex attribute, so the
        // index buffer is an ordinary array buffer the CPU sort refills each
        // time the camera moves. The attribute pointer is VAO state, so the VAO
        // has to be bound while it is set.
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, indexBuf_);
        glBufferData(GL_ARRAY_BUFFER, indexCount * sizeof(uint32_t),
                     indices.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, 0, nullptr);
        glVertexAttribDivisor(0, 1); // one index per instance, not per vertex
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);

        createTextureBuffers();

        // The CPU sort needs positions host-side, and pendingData_ is released
        // as soon as this returns.
        hostPositions_ = data.positions;
        sortKeys_.resize(splatCount_);
        sortIdx_.resize(splatCount_);
    }

    MGlobal::displayInfo(MString("[GaussianSplat] Uploaded ") + splatCount_ + " splats"
                         + (useCompute_ ? MString(" (sortN=") + (int)sortN_ + ")."
                                        : MString(" (GL 4.1 path, CPU sort).")));
}

// GL 4.1 path: depth-sort on the CPU and upload the resulting order.
// Produces the same ordering as the compute path — ascending view-space z,
// which is back-to-front for Maya's -Z-forward view space.
void GaussianRenderer::sortCPU(const MMatrix& wvm) {
    if (splatCount_ == 0 || hostPositions_.empty()) return;

    float f_wvm[16];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) f_wvm[i*4+j] = (float)wvm[i][j];

    if (!sortDirty_) {
        bool changed = false;
        for (int k = 0; k < 16 && !changed; ++k)
            if (std::abs(f_wvm[k] - prevWVM_[k]) > 1e-6f) changed = true;
        if (!changed) return;
    }
    sortDirty_ = false;
    std::memcpy(prevWVM_, f_wvm, sizeof(f_wvm));

    // View-space z under Maya's row-vector convention: z = p * M, i.e. only
    // the third column of the world-view matrix contributes. Matches what
    // depth.comp computes as (u_wvm * vec4(pos, 1.0)).z.
    const float m02 = f_wvm[0*4+2], m12 = f_wvm[1*4+2];
    const float m22 = f_wvm[2*4+2], m32 = f_wvm[3*4+2];

    const int n = splatCount_;
    sortKeys_.resize(n);
    sortIdx_.resize(n);
    for (int i = 0; i < n; ++i) {
        const float* p = &hostPositions_[(size_t)i * 4];
        const float z = p[0]*m02 + p[1]*m12 + p[2]*m22 + m32;
        sortKeys_[i] = depthToRadixKey(z);
        sortIdx_[i]  = (uint32_t)i;
    }

    radixSortByKey(sortKeys_, sortIdx_, sortKeysTmp_, sortIdxTmp_);

    glBindBuffer(GL_ARRAY_BUFFER, indexBuf_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)n * sizeof(uint32_t), sortIdx_.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GaussianRenderer::sort(const MMatrix& wvm) {
    if (splatCount_ == 0) return;

    // Build float matrix and check whether the camera moved since last sort.
    float f_wvm[16];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) f_wvm[i*4+j] = (float)wvm[i][j];

    if (!sortDirty_) {
        bool changed = false;
        for (int k = 0; k < 16 && !changed; ++k)
            if (std::abs(f_wvm[k] - prevWVM_[k]) > 1e-6f) changed = true;
        if (!changed) return;
    }
    sortDirty_ = false;
    std::memcpy(prevWVM_, f_wvm, sizeof(f_wvm));

    // 1. Calculate Depths
    glUseProgram(depthProgram_);
    glUniformMatrix4fv(depthUniforms_.wvm,       1, GL_FALSE, f_wvm);
    glUniform1ui(      depthUniforms_.numSplats,  splatCount_);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, posBuf_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, keyBuf_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, indexBuf_);

    uint32_t numGroups = (splatCount_ + 255) / 256;
    glDispatchCompute(numGroups, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // 2. GPU Bitonic Sort — iterative multi-pass over sortN_ entries
    if (!sortProgram_) return;

    glUseProgram(sortProgram_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, keyBuf_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, indexBuf_);
    glUniform1ui(sortUniforms_.numSplats, sortN_);

    uint32_t sortGroups = (sortN_ + 255) / 256;

    for (uint32_t p = 1; p < sortN_; p <<= 1) {
        for (uint32_t q = p; q >= 1; q >>= 1) {
            glUniform1ui(sortUniforms_.p, p);
            glUniform1ui(sortUniforms_.q, q);
            glDispatchCompute(sortGroups, 1, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        }
    }
}

void GaussianRenderer::draw(const MHWRender::MDrawContext& ctx, float splatScale, float opacityMult, int shDegree, const float camPos[3], bool sRGBToLinear, float gamma) {
    static std::once_flag glewOnce;
    std::call_once(glewOnce, []() {
        glewExperimental = GL_TRUE;
        GLenum err = glewInit();
        if (err != GLEW_OK)
            MGlobal::displayWarning(MString("[GaussianSplat] GLEW init: ") + (const char*)glewGetErrorString(err));
        else
            MGlobal::displayInfo("[GaussianSplat] GLEW initialized.");
    });
    // Needs a live context, so it cannot happen before the first draw. Must
    // run before uploadSplats, which picks its buffer target from the result.
    detectCapabilities();

    // Swap-and-release: move pendingData_ out under the lock (microseconds),
    // then upload outside the critical section so setPendingData() never blocks
    // on a multi-hundred-ms glBufferData transfer.
    SplatData uploadData;
    bool shouldUpload = false;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        if (newDataAvailable_) {
            uploadData = std::move(pendingData_);
            pendingData_ = SplatData();
            newDataAvailable_ = false;
            shouldUpload = true;
        }
    }
    if (shouldUpload) {
        uploadSplats(uploadData);
    }

    if (!isReady()) return;

    if (drawProgram_ == 0) {
        MGlobal::displayError("[GaussianSplat] drawProgram_ is 0 — shaders failed to compile!");
        return;
    }

    MMatrix wvm = ctx.getMatrix(MHWRender::MFrameContext::kWorldViewMtx);
    MMatrix pm  = ctx.getMatrix(MHWRender::MFrameContext::kProjectionMtx);

    if (useCompute_) sort(wvm); else sortCPU(wvm);

    float f_wvm[16], f_pm[16];
    for(int i=0; i<4; ++i) for(int j=0; j<4; ++j) {
        f_wvm[i*4+j] = (float)wvm[i][j];
        f_pm[i*4+j]  = (float)pm[i][j];
    }

    int x, y, w, h; ctx.getViewportDimensions(x, y, w, h);
    int viewport[4] = {x, y, w, h};

    glUseProgram(drawProgram_);
    // GL_FALSE: treat f_wvm as column-major. Since f_wvm is stored row-major (Maya convention),
    // GLSL receives the transpose of Maya's matrix, which is exactly what OpenGL column-vector
    // math needs: GLSL_mat = Maya_mat^T, so (GLSL_mat * col_vec) == (row_vec * Maya_mat).
    glUniformMatrix4fv(drawUniforms_.wvm,        1, GL_FALSE, f_wvm);
    glUniformMatrix4fv(drawUniforms_.pm,          1, GL_FALSE, f_pm);
    glUniform1f(       drawUniforms_.splatScale,  splatScale);
    glUniform1f(       drawUniforms_.opacityMult, opacityMult);
    glUniform4iv(      drawUniforms_.viewport,    1, viewport);
    // Cap requested degree by what's actually loaded on the GPU.
    int effectiveDegree = std::min(shDegree, loadedShDegree_);
    if (effectiveDegree < 0) effectiveDegree = 0;
    glUniform1i(       drawUniforms_.shDegree,           effectiveDegree);
    glUniform1i(       drawUniforms_.restFloatsPerSplat, restFloatsPerSplat_);
    glUniform1i(       drawUniforms_.sRGBToLinear,       sRGBToLinear ? 1 : 0);
    glUniform1f(       drawUniforms_.gamma,              gamma);
    glUniform3fv(      drawUniforms_.camPos,             1, camPos); // M2: precomputed in prepareForDraw

    glBindVertexArray(vao_);
    if (useCompute_) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, posBuf_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, rotBuf_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, sclBuf_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, shBuf_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, indexBuf_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, shRestBuf_);
    } else {
        // Texture units 0..4 stand in for SSBO bindings 0..3 and 6; the sorted
        // index comes from the instanced vertex attribute set up in uploadSplats.
        const struct { GLuint tex; GLint loc; } binds[] = {
            { posTex_,    texUniforms_.pos    },
            { rotTex_,    texUniforms_.rot    },
            { sclTex_,    texUniforms_.scl    },
            { shTex_,     texUniforms_.sh     },
            { shRestTex_, texUniforms_.shRest },
        };
        for (int unit = 0; unit < 5; ++unit) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_BUFFER, binds[unit].tex);
            glUniform1i(binds[unit].loc, unit);
        }
        glActiveTexture(GL_TEXTURE0);
    }

    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, splatCount_);

    // Check for GL errors after draw
    GLenum glErr = glGetError();
    if (glErr != GL_NO_ERROR) {
        static bool errReported = false;
        if (!errReported) {
            errReported = true;
            MGlobal::displayError(MString("[GaussianSplat] GL error after draw: ") + (int)glErr);
        }
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

// The shader sources carry no #version line of their own; `preamble` supplies
// it along with any feature defines, so one source file can serve both the 4.3
// and 4.1 paths. The trailing #line 1 keeps compiler diagnostics numbered
// against the file on disk rather than against the concatenation.
GLuint GaussianRenderer::loadShader(GLenum type, const char* path, const std::string& preamble) {
    std::ifstream file(path);
    if (!file) {
        MGlobal::displayError(MString("[GaussianSplat] Cannot open shader: ") + path);
        return 0;
    }
    std::stringstream ss; ss << file.rdbuf();
    std::string src = preamble + "#line 1\n" + ss.str();
    const char* srcPtr = src.c_str();
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &srcPtr, nullptr);
    glCompileShader(s);

    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log(len > 0 ? len : 1, '\0');
        glGetShaderInfoLog(s, len, nullptr, &log[0]);
        MGlobal::displayError(MString("[GaussianSplat] Shader compile error (") + path + "):\n" + log.c_str());
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint GaussianRenderer::linkProgram(GLuint vert, GLuint frag, GLuint comp) {
    GLuint p = glCreateProgram();
    if (vert) glAttachShader(p, vert);
    if (frag) glAttachShader(p, frag);
    if (comp) glAttachShader(p, comp);
    glLinkProgram(p);

    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::string log(len > 0 ? len : 1, '\0');
        glGetProgramInfoLog(p, len, nullptr, &log[0]);
        MGlobal::displayError(MString("[GaussianSplat] Program link error:\n") + log.c_str());
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

void GaussianRenderer::buildShaderProgram() {
    std::string vPath = s_shaderDir + "gaussian.vert";
    std::string fPath = s_shaderDir + "gaussian.frag";
    // GS_USE_SSBO switches gaussian.vert between shader storage buffers and
    // the texture-buffer reads that OpenGL 4.1 is limited to.
    const std::string preamble = useCompute_
        ? "#version 450\n#define GS_USE_SSBO 1\n"
        : "#version 410 core\n";
    GLuint v = loadShader(GL_VERTEX_SHADER, vPath.c_str(), preamble);
    GLuint f = loadShader(GL_FRAGMENT_SHADER, fPath.c_str(), preamble);
    if (v && f) {
        if (drawProgram_) glDeleteProgram(drawProgram_); // H1: release old program
        drawProgram_ = linkProgram(v, f);
        if (drawProgram_) {
            // M1: cache uniform locations once after linking
            drawUniforms_.wvm        = glGetUniformLocation(drawProgram_, "u_wvm");
            drawUniforms_.pm         = glGetUniformLocation(drawProgram_, "u_pm");
            drawUniforms_.splatScale = glGetUniformLocation(drawProgram_, "u_splatScale");
            drawUniforms_.opacityMult= glGetUniformLocation(drawProgram_, "u_opacityMult");
            drawUniforms_.viewport           = glGetUniformLocation(drawProgram_, "u_viewport");
            drawUniforms_.shDegree           = glGetUniformLocation(drawProgram_, "u_shDegree");
            drawUniforms_.restFloatsPerSplat = glGetUniformLocation(drawProgram_, "u_restFloatsPerSplat");
            drawUniforms_.sRGBToLinear       = glGetUniformLocation(drawProgram_, "u_sRGBToLinear");
            drawUniforms_.gamma              = glGetUniformLocation(drawProgram_, "u_gamma");
            drawUniforms_.camPos             = glGetUniformLocation(drawProgram_, "u_camPos");
            // Present only on the GL 4.1 path; -1 elsewhere, which glUniform1i ignores.
            texUniforms_.pos    = glGetUniformLocation(drawProgram_, "u_posTex");
            texUniforms_.rot    = glGetUniformLocation(drawProgram_, "u_rotTex");
            texUniforms_.scl    = glGetUniformLocation(drawProgram_, "u_sclTex");
            texUniforms_.sh     = glGetUniformLocation(drawProgram_, "u_shTex");
            texUniforms_.shRest = glGetUniformLocation(drawProgram_, "u_shRestTex");
        }
    }
    if (v) glDeleteShader(v);
    if (f) glDeleteShader(f);
}

void GaussianRenderer::buildSortProgram() {
    // The 4.1 path sorts on the CPU and has no compute shaders to build.
    if (!useCompute_) return;

    const std::string preamble = "#version 450\n";
    std::string dPath = s_shaderDir + "depth.comp";
    std::string sPath = s_shaderDir + "sort.comp";
    GLuint d = loadShader(GL_COMPUTE_SHADER, dPath.c_str(), preamble);
    if (d) {
        if (depthProgram_) glDeleteProgram(depthProgram_); // H1
        depthProgram_ = linkProgram(0, 0, d);
        if (depthProgram_) {
            depthUniforms_.wvm      = glGetUniformLocation(depthProgram_, "u_wvm");
            depthUniforms_.numSplats= glGetUniformLocation(depthProgram_, "u_numSplats");
        }
    }
    GLuint s = loadShader(GL_COMPUTE_SHADER, sPath.c_str(), preamble);
    if (s) {
        if (sortProgram_) glDeleteProgram(sortProgram_); // H1
        sortProgram_ = linkProgram(0, 0, s);
        if (sortProgram_) {
            sortUniforms_.numSplats = glGetUniformLocation(sortProgram_, "u_numSplats");
            sortUniforms_.p         = glGetUniformLocation(sortProgram_, "u_p");
            sortUniforms_.q         = glGetUniformLocation(sortProgram_, "u_q");
        }
    }
    if (d) glDeleteShader(d);
    if (s) glDeleteShader(s);
}
