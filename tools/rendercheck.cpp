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
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    glDrawArraysInstanced(GL_TRIANGLES,0,6,N);

    GLenum err=glGetError();
    std::vector<unsigned char> px(W*H*4);
    glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,px.data());

    long lit=0; int mr=0,mg=0,mb=0;
    for(int i=0;i<W*H;++i){ if(px[i*4]|px[i*4+1]|px[i*4+2]) ++lit;
        mr=std::max(mr,(int)px[i*4]); mg=std::max(mg,(int)px[i*4+1]); mb=std::max(mb,(int)px[i*4+2]); }
    const unsigned char* c=&px[(H/2*W+W/2)*4];

    printf("GL error after draw : 0x%x %s\n",err,err?"<-- PROBLEM":"(none)");
    printf("lit pixels          : %ld / %d\n",lit,W*H);
    printf("max channel R/G/B   : %d / %d / %d\n",mr,mg,mb);
    printf("center pixel RGBA   : %d %d %d %d\n",c[0],c[1],c[2],c[3]);
    bool pass = (err==0) && lit>50 && mr>40 && mg>40 && mb>40;
    printf("\n%s\n", pass?"RESULT: PASS — all three splats rendered through the 4.1 path"
                        :"RESULT: FAILED");
    return pass?0:1;
}
