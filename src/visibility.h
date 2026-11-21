/* The source has already been normalized in the producer. Preserve the
 * existing all-zero admission rule with an occlusion query and no color writes. */
static GLuint visibility_fbo;
static PFNGLFRAMEBUFFERPARAMETERIPROC framebuffer_parameter;
static GLint repair_uniform;
static GLuint tile_program;
/* One fragment covers a tile. An early return accepts that tile on the
 * first nonzero component. A discard is only possible after all texels
 * tested zero. Clamp the partial right/bottom tiles to the image edge. */
static const char tile_fragment[] =
    "#version 130\n"
    "uniform sampler2D src;\n"
    "void main() {\n"
    "  ivec2 base = ivec2(gl_FragCoord.xy) * 4;\n"
    "  ivec2 last = textureSize(src, 0) - 1;\n"
    "  for (int y = 0; y < 4; ++y)\n"
    "    for (int x = 0; x < 4; ++x)\n"
    "      if (any(notEqual(texelFetch(src, min(base+ivec2(x,y),last), 0), vec4(0)))) return;\n"
    "  discard;\n"
    "}";
static int visibility_w,visibility_h;

static int visibility_size(int w,int h)
{
    BindFramebuffer(GL_FRAMEBUFFER,visibility_fbo);
    if(w==visibility_w && h==visibility_h) return 1;
    visibility_w=visibility_h=0;
    framebuffer_parameter(GL_FRAMEBUFFER,GL_FRAMEBUFFER_DEFAULT_WIDTH,w);
    framebuffer_parameter(GL_FRAMEBUFFER,GL_FRAMEBUFFER_DEFAULT_HEIGHT,h);
    GLenum status=CheckFramebufferStatus(GL_FRAMEBUFFER);
    if(GetError()!=GL_NO_ERROR || status!=GL_FRAMEBUFFER_COMPLETE) return 0;
    visibility_w=w;
    visibility_h=h;
    return 1;
}

static int init_visibility(void)
{
    const char *ext=(const char *)GetString(GL_EXTENSIONS);
    if(!ext || !strstr(ext,"GL_ARB_framebuffer_no_attachments")) return 0;
    framebuffer_parameter=(PFNGLFRAMEBUFFERPARAMETERIPROC)eglGetProcAddress("glFramebufferParameteri");
    __typeof__(&glDrawBuffer) draw_buffer=(__typeof__(&glDrawBuffer))eglGetProcAddress("glDrawBuffer");
    __typeof__(&glReadBuffer) read_buffer=(__typeof__(&glReadBuffer))eglGetProcAddress("glReadBuffer");
    if(!framebuffer_parameter || !draw_buffer || !read_buffer) return 0;
    GenFramebuffers(1,&visibility_fbo);
    BindFramebuffer(GL_FRAMEBUFFER,visibility_fbo);
    draw_buffer(GL_NONE);
    read_buffer(GL_NONE);
    repair_uniform=GetUniformLocation(program,"repair");
    int ok=visibility_fbo && visibility_size(1,1);
    BindFramebuffer(GL_FRAMEBUFFER,fbo);
    if(ok) {
        tile_program=CreateProgram();
        GLint linked=0;
        if(tile_program && shader(tile_program,GL_VERTEX_SHADER,fullscreen_vertex) &&
           shader(tile_program,GL_FRAGMENT_SHADER,tile_fragment)) {
            LinkProgram(tile_program);
            GetProgramiv(tile_program,GL_LINK_STATUS,&linked);
        }
        if(!linked) {
            PFNGLDELETEPROGRAMPROC del=(PFNGLDELETEPROGRAMPROC)eglGetProcAddress("glDeleteProgram");
            if(tile_program && del) del(tile_program);
            tile_program=0;
        }
        fprintf(stderr,"visibility tiled visibility=%d\n",tile_program!=0);
    }
    return ok;
}

static void (*visibility_pending)(void);
static int texture_has_color(GLuint tex,int w,int h)
{
    if(!visibility_size(w,h)) return 0;
    BindTexture(GL_TEXTURE_2D,tex);
    if(tile_program) {
        UseProgram(tile_program);
        Viewport(0,0,(w+3)/4,(h+3)/4);
    } else {
        UseProgram(program);
        Uniform1i(repair_uniform,0);
        Viewport(0,0,w,h);
    }
    GLuint passed=0;
    BeginQuery(GL_ANY_SAMPLES_PASSED,query);
    DrawArrays(GL_TRIANGLES,0,3);
    EndQuery(GL_ANY_SAMPLES_PASSED);
    if(visibility_pending) visibility_pending();
    GetQueryObjectuiv(query,GL_QUERY_RESULT,&passed);
    return GetError()==GL_NO_ERROR && passed;
}
