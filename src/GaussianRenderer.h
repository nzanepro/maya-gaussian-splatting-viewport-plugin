#pragma once
#include "PlyLoader.h"
#include <GL/glew.h>
#include <maya/MMatrix.h>
#include <maya/MPoint.h>
#include <maya/MDrawContext.h>
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

class GaussianRenderer {
public:
    GaussianRenderer();
    ~GaussianRenderer();

    static void setShaderDir(const std::string& dir) { s_shaderDir = dir; }

    void setPendingData(const SplatData& data) {
        std::lock_guard<std::mutex> lock(dataMutex_);
        pendingData_ = data;
        newDataAvailable_ = true;
    }

    void sort(const MMatrix& viewMatrix);
    void sortCPU(const MMatrix& viewMatrix);
    void draw(const MHWRender::MDrawContext& ctx,
              float        splatScale,
              float        opacityMult,
              int          shDegree,
              const float  camPos[3],
              bool         sRGBToLinear,
              float        gamma);

    int loadedShDegree() const { return loadedShDegree_; }

    // True when the context provides OpenGL 4.3 compute shaders + SSBOs.
    // False selects the OpenGL 4.1 fallback (texture buffers + CPU sort),
    // which is the only path available on macOS.
    bool usesComputePath() const { return useCompute_; }

    bool isReady() const { return vao_ != 0 && splatCount_ > 0; }
    int  splatCount() const { return splatCount_; }

private:
    void initGL();
    void detectCapabilities();
    void buildShaderProgram();
    void buildSortProgram();
    void destroyGL();
    void uploadSplats(const SplatData& data);
    void createTextureBuffers();

    GLuint loadShader(GLenum type, const char* path, const std::string& preamble);
    GLuint linkProgram(GLuint vert, GLuint frag, GLuint comp = 0);

    GLuint vao_          = 0;
    GLuint posBuf_       = 0;
    GLuint rotBuf_       = 0;
    GLuint sclBuf_       = 0;
    GLuint shBuf_        = 0;
    GLuint shRestBuf_    = 0; // SH degree-1..3 coefficients (binding 6)
    GLuint indexBuf_     = 0; // Sorted indices
    GLuint keyBuf_       = 0; // Depths for sorting (compute path only)

    // GL 4.1 path: the buffers above re-exposed as texture buffers, since
    // OpenGL 4.1 has no shader storage buffers.
    GLuint posTex_       = 0;
    GLuint rotTex_       = 0;
    GLuint sclTex_       = 0;
    GLuint shTex_        = 0;
    GLuint shRestTex_    = 0;
    GLuint drawProgram_  = 0;
    GLuint sortProgram_  = 0;
    GLuint depthProgram_ = 0;

    // Cached uniform locations — populated once after shader link (M1).
    struct {
        GLint wvm = -1, pm = -1, splatScale = -1, opacityMult = -1;
        GLint viewport = -1, shDegree = -1, restFloatsPerSplat = -1, camPos = -1;
        GLint sRGBToLinear = -1, gamma = -1;
    } drawUniforms_;
    struct {
        GLint wvm = -1, numSplats = -1;
    } depthUniforms_;
    struct {
        GLint numSplats = -1, p = -1, q = -1;
    } sortUniforms_;
    // GL 4.1 path: samplerBuffer uniforms standing in for the SSBO bindings.
    struct {
        GLint pos = -1, rot = -1, scl = -1, sh = -1, shRest = -1;
    } texUniforms_;

    int      splatCount_         = 0;
    uint32_t sortN_              = 0;
    int      loadedShDegree_     = 0; // 0..3 — highest SH degree available on GPU
    int      restFloatsPerSplat_ = 0; // 0, 9, 24, or 45

    // Sort-on-move: skip dispatch when camera hasn't changed.
    float prevWVM_[16]   = {};
    bool  sortDirty_     = true; // force sort after new data upload

    // Capability-selected render path. Assume the compute path until a GL
    // context exists and detectCapabilities() can ask; nothing touches GL
    // before then.
    bool   useCompute_   = true;
    bool   capsChecked_  = false;
    GLenum dataTarget_   = GL_SHADER_STORAGE_BUFFER; // upload binding point

    // GL 4.1 path only: positions are kept host-side so the depth sort can
    // run on the CPU, plus scratch buffers reused across frames to keep the
    // per-frame sort allocation-free.
    std::vector<float>    hostPositions_;
    std::vector<uint32_t> sortKeys_;
    std::vector<uint32_t> sortIdx_;
    std::vector<uint32_t> sortKeysTmp_;
    std::vector<uint32_t> sortIdxTmp_;

    SplatData  pendingData_;
    bool       newDataAvailable_ = false;
    std::mutex dataMutex_;

    static std::string s_shaderDir;
};
