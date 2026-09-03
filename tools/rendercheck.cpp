// End-to-end exercise of the GL 4.1 path: builds the real shaders, sets up the
// same texture buffers and instanced index attribute GaussianRenderer uses,
// renders a synthetic 3-splat scene into an FBO and reads the pixels back.
#include <GL/glew.h>
#include <OpenGL/OpenGL.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static std::string slurp(const std::string& p){ std::ifstream f(p); std::stringstream s; s<<f.rdbuf(); return s.str(); }

static GLuint mkShader(GLenum t, const std::string& path){
    std::string src = "#version 410 core\n#line 1\n" + slurp(path);
    const char* p = src.c_str();
    GLuint s = glCreateShader(t); glShaderSource(s,1,&p,nullptr); glCompileShader(s);
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){ char log[4096]; glGetShaderInfoLog(s,4096,nullptr,log); printf("compile fail %s:\n%s\n",path.c_str(),log); exit(1);} 
    return s;
}

int main(int argc, char** argv){
    const std::string dir = argc>1?argv[1]:"src/shaders/";
    CGLPixelFormatAttribute attrs[] = { kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
                                        kCGLPFAAccelerated,(CGLPixelFormatAttribute)0 };
    CGLPixelFormatObj pix; GLint n; CGLChoosePixelFormat(attrs,&pix,&n);
    CGLContextObj ctx; CGLCreateContext(pix,nullptr,&ctx); CGLSetCurrentContext(ctx);
    glewExperimental=GL_TRUE; glewInit(); while(glGetError()!=GL_NO_ERROR){}

    const int W=64,H=64;
    GLuint fbo,color; glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glGenTextures(1,&color); glBindTexture(GL_TEXTURE_2D,color);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){printf("FBO incomplete\n");return 1;}

    GLuint prog=glCreateProgram();
    glAttachShader(prog,mkShader(GL_VERTEX_SHADER,dir+"gaussian.vert"));
    glAttachShader(prog,mkShader(GL_FRAGMENT_SHADER,dir+"gaussian.frag"));
    glLinkProgram(prog);
    GLint ok=0; glGetProgramiv(prog,GL_LINK_STATUS,&ok);
    if(!ok){char log[4096];glGetProgramInfoLog(prog,4096,nullptr,log);printf("link fail:\n%s\n",log);return 1;}

    // 3 splats along the view axis, so the depth sort has something to order.
    const int N=3;
    float positions[N*4]={ 0,0,0,1,   0.6f,0,-1,1,  -0.6f,0,1,1 };
    float rotations[N*4]={ 1,0,0,0,   1,0,0,0,      1,0,0,0 };
    float scales   [N*4]={ .4f,.4f,.4f,1.f,  .4f,.4f,.4f,1.f,  .4f,.4f,.4f,1.f };
    float sh_dc    [N*4]={ 1.77f,0,0,0,  0,1.77f,0,0,  0,0,1.77f,0 };
    float shRest   [1]  ={ 0.f };
    unsigned idx   [N]  ={ 0,1,2 };

    GLuint vao; glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    auto mkbuf=[&](const void* d,size_t bytes){ GLuint b; glGenBuffers(1,&b);
        glBindBuffer(GL_TEXTURE_BUFFER,b); glBufferData(GL_TEXTURE_BUFFER,bytes,d,GL_STATIC_DRAW); return b; };
    GLuint pb=mkbuf(positions,sizeof positions), rb=mkbuf(rotations,sizeof rotations),
           sb=mkbuf(scales,sizeof scales),       hb=mkbuf(sh_dc,sizeof sh_dc),
           rr=mkbuf(shRest,sizeof shRest);
    auto mktex=[&](GLuint buf,GLenum fmt){ GLuint t; glGenTextures(1,&t);
        glBindTexture(GL_TEXTURE_BUFFER,t); glTexBuffer(GL_TEXTURE_BUFFER,fmt,buf); return t; };
    GLuint pt=mktex(pb,GL_RGBA32F),rt=mktex(rb,GL_RGBA32F),st=mktex(sb,GL_RGBA32F),
           ht=mktex(hb,GL_RGBA32F),rtt=mktex(rr,GL_R32F);

    GLuint ib; glGenBuffers(1,&ib);
    glBindBuffer(GL_ARRAY_BUFFER,ib); glBufferData(GL_ARRAY_BUFFER,sizeof idx,idx,GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribIPointer(0,1,GL_UNSIGNED_INT,0,nullptr);
    glVertexAttribDivisor(0,1);

    glUseProgram(prog);
    float wvm[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,-5,1};
    const float fov=1.0f/std::tan(0.5f*45.f*3.14159265f/180.f), nr=0.1f,fr=100.f;
    float pm[16]={fov,0,0,0, 0,fov,0,0, 0,0,(fr+nr)/(nr-fr),-1, 0,0,(2*fr*nr)/(nr-fr),0};
    int vp[4]={0,0,W,H}; float cam[3]={0,0,5};
    glUniformMatrix4fv(glGetUniformLocation(prog,"u_wvm"),1,GL_FALSE,wvm);
    glUniformMatrix4fv(glGetUniformLocation(prog,"u_pm"),1,GL_FALSE,pm);
    glUniform1f(glGetUniformLocation(prog,"u_splatScale"),1.f);
    glUniform1f(glGetUniformLocation(prog,"u_opacityMult"),1.f);
    glUniform4iv(glGetUniformLocation(prog,"u_viewport"),1,vp);
    glUniform1i(glGetUniformLocation(prog,"u_shDegree"),0);
    glUniform1i(glGetUniformLocation(prog,"u_restFloatsPerSplat"),0);
    glUniform1i(glGetUniformLocation(prog,"u_sRGBToLinear"),0);
    glUniform1f(glGetUniformLocation(prog,"u_gamma"),1.f);
    glUniform3fv(glGetUniformLocation(prog,"u_camPos"),1,cam);
    const char* tn[5]={"u_posTex","u_rotTex","u_sclTex","u_shTex","u_shRestTex"};
    GLuint tex[5]={pt,rt,st,ht,rtt};
    for(int u=0;u<5;++u){ glActiveTexture(GL_TEXTURE0+u); glBindTexture(GL_TEXTURE_BUFFER,tex[u]);
                          glUniform1i(glGetUniformLocation(prog,tn[u]),u); }
    glActiveTexture(GL_TEXTURE0);

    glViewport(0,0,W,H);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);

    GLint uCullEn = glGetUniformLocation(prog,"u_cullEnabled");
    GLint uCullIv = glGetUniformLocation(prog,"u_cullInvert");
    GLint uCullM  = glGetUniformLocation(prog,"u_cullBoxInv");
    GLint uStride = glGetUniformLocation(prog,"u_displayStride");

    struct Res { GLenum err; long lit; int mr,mg,mb; };
    auto pass_draw = [&](int cullEnabled, int cullInvert, const float* boxInv, int stride){
        glUniform1i(uCullEn, cullEnabled);
        glUniform1i(uCullIv, cullInvert);
        glUniform1i(uStride, stride);
        if (boxInv) glUniformMatrix4fv(uCullM, 1, GL_FALSE, boxInv);
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        glDrawArraysInstanced(GL_TRIANGLES,0,6,N);
        Res r{}; r.err = glGetError();
        std::vector<unsigned char> px(W*H*4);
        glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,px.data());
        for(int i=0;i<W*H;++i){ if(px[i*4]|px[i*4+1]|px[i*4+2]) ++r.lit;
            r.mr=std::max(r.mr,(int)px[i*4]); r.mg=std::max(r.mg,(int)px[i*4+1]);
            r.mb=std::max(r.mb,(int)px[i*4+2]); }
        return r;
    };

    // Splat 0 is red at the origin, 1 is green at x=+0.6, 2 is blue at x=-0.6.
    // A unit cube scaled to 0.5 about the origin therefore contains only red.
    // Its inverse world matrix is diag(2,2,2,1), written Maya row-major and
    // uploaded with GL_FALSE exactly as GaussianDrawOverride does.
    const float boxInv[16] = {2,0,0,0, 0,2,0,0, 0,0,2,0, 0,0,0,1};

    Res all   = pass_draw(0,0,boxInv,1);
    Res inside= pass_draw(1,0,boxInv,1);
    Res outsd = pass_draw(1,1,boxInv,1);
    Res thin  = pass_draw(0,0,boxInv,2);   // every 2nd splat -> drops green

    printf("no cull      : err 0x%x  lit %5ld  RGB %3d/%3d/%3d\n", all.err,all.lit,all.mr,all.mg,all.mb);
    printf("cull to box  : err 0x%x  lit %5ld  RGB %3d/%3d/%3d   (expect red only)\n",
           inside.err,inside.lit,inside.mr,inside.mg,inside.mb);
    printf("cull inverted: err 0x%x  lit %5ld  RGB %3d/%3d/%3d   (expect green+blue)\n",
           outsd.err,outsd.lit,outsd.mr,outsd.mg,outsd.mb);
    printf("stride 2     : err 0x%x  lit %5ld  RGB %3d/%3d/%3d   (expect red+blue)\n",
           thin.err,thin.lit,thin.mr,thin.mg,thin.mb);

    // 3DGS evaluates colour as C0*dc + 0.5, so a channel with dc=0 still lands
    // at ~0.5 (127/255). "Present" therefore means clearly above that floor,
    // not near zero.
    const int HI = 200, LO = 160;
    // With all three overlapping, each colour is diluted by the others, so the
    // bar here is only "clearly above the 0.5 floor".
    bool ok_all    = all.err==0 && all.lit>50 && all.mr>LO && all.mg>LO && all.mb>LO;
    bool ok_inside = inside.err==0 && inside.mr>HI && inside.mg<LO && inside.mb<LO;
    bool ok_outsd  = outsd.err==0  && outsd.mr<LO  && outsd.mg>HI && outsd.mb>HI;
    bool ok_thin   = thin.err==0   && thin.mr>HI   && thin.mg<LO  && thin.mb>HI;
    printf("\n  all three render        : %s\n", ok_all?"PASS":"FAIL");
    printf("  cull keeps inside only  : %s\n", ok_inside?"PASS":"FAIL");
    printf("  invert keeps outside    : %s\n", ok_outsd?"PASS":"FAIL");
    printf("  stride drops every 2nd  : %s\n", ok_thin?"PASS":"FAIL");
    bool pass = ok_all && ok_inside && ok_outsd && ok_thin;
    printf("\n%s\n", pass?"RESULT: PASS — 4.1 path, cull box and display stride all correct"
                        :"RESULT: FAILED");
    return pass?0:1;
}
