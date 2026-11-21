/* The app's exported snapshot is private until publish is acknowledged.
 * Convert its GL bottom row to Android row zero and reproduce the shim's
 * alpha repair, both on GPU. Only the nonempty bit crosses to the CPU. */
#define INPUT_GL(X) \
 X(PFNGLCREATESHADERPROC,CreateShader) X(PFNGLSHADERSOURCEPROC,ShaderSource) \
 X(PFNGLCOMPILESHADERPROC,CompileShader) X(PFNGLGETSHADERIVPROC,GetShaderiv) \
 X(PFNGLDELETESHADERPROC,DeleteShader) X(PFNGLCREATEPROGRAMPROC,CreateProgram) \
 X(PFNGLATTACHSHADERPROC,AttachShader) X(PFNGLLINKPROGRAMPROC,LinkProgram) \
 X(PFNGLGETPROGRAMIVPROC,GetProgramiv) X(PFNGLUSEPROGRAMPROC,UseProgram) \
 X(PFNGLGETUNIFORMLOCATIONPROC,GetUniformLocation) X(PFNGLUNIFORM1IPROC,Uniform1i) \
 X(PFNGLGENQUERIESPROC,GenQueries) X(PFNGLBEGINQUERYPROC,BeginQuery) \
 X(PFNGLENDQUERYPROC,EndQuery) X(PFNGLGETQUERYOBJECTUIVPROC,GetQueryObjectuiv) \
 X(__typeof__(&glClear),Clear) X(__typeof__(&glViewport),Viewport) \
 X(__typeof__(&glTexImage2D),TexImage2D) X(__typeof__(&glTexParameteri),TexParameteri) \
 X(__typeof__(&glCopyTexSubImage2D),CopyTexSubImage2D) X(__typeof__(&glFinish),Finish) \
 X(__typeof__(&glDrawArrays),DrawArrays)
INPUT_GL(DECLARE)
static GLuint program,fbo,query;
static int shader(GLuint dst,GLenum type,const char *source)
{
    GLuint sh=CreateShader(type);
    ShaderSource(sh,1,&source,NULL);
    CompileShader(sh);
    GLint ok=0;
    GetShaderiv(sh,GL_COMPILE_STATUS,&ok);
    if(ok) AttachShader(dst,sh);
    DeleteShader(sh);
    return ok;
}
static const char fullscreen_vertex[]="#version 130\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.-1.,0,1);}";
static int init_input(void)
{
#define LAYER_INPUT_LOAD(type,name) name=(type)eglGetProcAddress("gl" #name); if(!name) return 0;
    INPUT_GL(LAYER_INPUT_LOAD)
    program=CreateProgram();
    if(!shader(program,GL_VERTEX_SHADER,fullscreen_vertex) ||
       !shader(program,GL_FRAGMENT_SHADER,"#version 130\nuniform sampler2D src; uniform int repair; out vec4 color; void main(){ivec2 p=ivec2(gl_FragCoord.xy);p.y=textureSize(src,0).y-1-p.y;color=texelFetch(src,p,0);if(all(equal(color,vec4(0))))discard;if(repair!=0 && color.a==0.)color.a=1.;}")) return 0;
    LinkProgram(program);
    GLint ok;
    GetProgramiv(program,GL_LINK_STATUS,&ok);
    if(!ok) return 0;
    GenFramebuffers(1,&fbo);
    GenQueries(1,&query);
    return GetError()==GL_NO_ERROR;
}
static GLuint import_texture(int fd,int w,int h)
{
    GLuint mem=0,tex=0;
    GLint dedicated=1;
    CreateMemoryObjectsEXT(1,&mem);
    MemoryObjectParameterivEXT(mem,GL_DEDICATED_MEMORY_OBJECT_EXT,&dedicated);
    ImportMemoryFdEXT(mem,(uint64_t)w*h*4,GL_HANDLE_TYPE_OPAQUE_FD_EXT,fd);
    GenTextures(1,&tex);
    BindTexture(GL_TEXTURE_2D,tex);
    TexStorageMem2DEXT(GL_TEXTURE_2D,1,GL_RGBA8,w,h,mem,0);
    DeleteMemoryObjectsEXT(1,&mem);
    TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    if(GetError()!=GL_NO_ERROR) {DeleteTextures(1,&tex);return 0;}
    return tex;
}
static GLuint normalized(int fd,int repair,int w,int h,int allow_empty,int *confirmed_empty)
{
    /* Retain a bounded set of scratch textures for interleaved window sizes. */
    static struct {GLuint tex;int w,h;} cache[8];
    static unsigned next;
    unsigned i=0;
    while(i<8 && (cache[i].w!=w || cache[i].h!=h)) ++i;
    if(i==8) {
        i=next++%8;
        if(!cache[i].tex) GenTextures(1,&cache[i].tex);
        BindTexture(GL_TEXTURE_2D,cache[i].tex);
        TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
        if(GetError()!=GL_NO_ERROR) {cache[i].w=0;close(fd);return 0;}
        cache[i].w=w;cache[i].h=h;
    }
    GLuint tex=import_texture(fd,w,h),visible=0;
    if(!tex) return 0;
    BindFramebuffer(GL_FRAMEBUFFER,fbo);
    FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,cache[i].tex,0);
    if(CheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) {DeleteTextures(1,&tex);return 0;}
    UseProgram(program);
    Uniform1i(GetUniformLocation(program,"repair"),repair);
    Viewport(0,0,w,h);
    Clear(GL_COLOR_BUFFER_BIT);
    BeginQuery(GL_ANY_SAMPLES_PASSED,query);
    DrawArrays(GL_TRIANGLES,0,3);
    EndQuery(GL_ANY_SAMPLES_PASSED);
    GetQueryObjectuiv(query,GL_QUERY_RESULT,&visible);
    if(visible || allow_empty) CopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,w,h);
    Finish();
    GLenum error=GetError();
    if((!visible && !allow_empty) || error!=GL_NO_ERROR) {
        if(confirmed_empty && !visible && !allow_empty && error==GL_NO_ERROR)
            *confirmed_empty=1;
        DeleteTextures(1,&tex);
        return 0;
    }
    return tex;
}
