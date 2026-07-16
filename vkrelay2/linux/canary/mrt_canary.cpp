// MRT canary: proves MULTIPLE color attachments render correctly through the relay -- the
// productionized development probe that exposed the silent 2-color half-render (FBO COMPLETE,
// attachment 0 correct, attachment 1 silently black). GLX compat context (the proven-through-relay
// bring-up), then three sub-gates:
//   A. 2-attachment distinct-color draw: one fullscreen triangle, a GLSL-1.20 shader writing
//      distinct colors to gl_FragData[0]/[1]; both attachments read back exactly.
//   B. per-attachment write mask (glColorMaski on attachment 1): proves the faithful blend-state
//      ARRAY actually applies per attachment (independentBlend riding the honest host feature).
//   C. UNUSED gap: glDrawBuffers {ATT0, GL_NONE, ATT2} over a 3-attachment FBO -- zink emits a
//      VK_ATTACHMENT_UNUSED color-ref hole (the wire's -1); the gap attachment must stay at its
//      clear color while 0 and 2 render.
// Self-judging, greppable: run_mrt_smoke.sh gates on the printed pixels + PASS.
#include <GL/gl.h>
#include <GL/glx.h>
#include <cstdio>
#include <cstring>

typedef GLXContext (*PFNCCA)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
typedef void (*PFNGENFB)(GLsizei, GLuint*);
typedef void (*PFNBINDFB)(GLenum, GLuint);
typedef void (*PFNGENRB)(GLsizei, GLuint*);
typedef void (*PFNBINDRB)(GLenum, GLuint);
typedef void (*PFNRBSTORE)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (*PFNFBRB)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (*PFNCHECKFB)(GLenum);
typedef void (*PFNDRAWBUFS)(GLsizei, const GLenum*);
typedef void (*PFNCOLORMASKI)(GLuint, GLboolean, GLboolean, GLboolean, GLboolean);
typedef GLuint (*PFNCREATESH)(GLenum);
typedef void (*PFNSHSRC)(GLuint, GLsizei, const char* const*, const GLint*);
typedef void (*PFNCOMPILE)(GLuint);
typedef void (*PFNGETSHIV)(GLuint, GLenum, GLint*);
typedef GLuint (*PFNCREATEPROG)(void);
typedef void (*PFNATTACH)(GLuint, GLuint);
typedef void (*PFNLINK)(GLuint);
typedef void (*PFNGETPROGIV)(GLuint, GLenum, GLint*);
typedef void (*PFNUSEPROG)(GLuint);

#define GL_FRAMEBUFFER_C 0x8D40
#define GL_RENDERBUFFER_C 0x8D41
#define GL_COLOR_ATTACHMENT0_C 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE_C 0x8CD5
#define GL_FRAGMENT_SHADER_C 0x8B30
#define GL_VERTEX_SHADER_C 0x8B31
#define GL_COMPILE_STATUS_C 0x8B81
#define GL_LINK_STATUS_C 0x8B82
#define GL_MAX_DRAW_BUFFERS_C 0x8824
#define GL_NONE_C 0

namespace {

void* gp(const char* n) {
    return reinterpret_cast<void*>(glXGetProcAddressARB(reinterpret_cast<const GLubyte*>(n)));
}

// One shader writes three distinct colors; unbound draw buffers discard the extra outputs.
const char* kVS = "#version 120\n"
                  "void main() { gl_Position = gl_Vertex; }\n";
const char* kFS = "#version 120\n"
                  "void main() {\n"
                  "  gl_FragData[0] = vec4(0.8, 0.2, 0.6, 1.0);\n" /* cc3399ff */
                  "  gl_FragData[1] = vec4(0.2, 0.6, 0.8, 1.0);\n" /* 3399ccff */
                  "  gl_FragData[2] = vec4(0.6, 0.8, 0.2, 1.0);\n" /* 99cc33ff */
                  "}\n";

bool read_att(int index, unsigned char (&px)[4]) {
    glReadBuffer(static_cast<GLenum>(GL_COLOR_ATTACHMENT0_C + index));
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return true;
}

bool px_is(const unsigned char (&px)[4], unsigned r, unsigned g, unsigned b, const char* what) {
    const bool ok = px[0] == r && px[1] == g && px[2] == b;
    std::printf("MRT-CANARY: %s pixel=%02x%02x%02x expected=%02x%02x%02x %s\n", what, px[0], px[1],
                px[2], r, g, b, ok ? "OK" : "WRONG");
    return ok;
}

void fullscreen_triangle() {
    glBegin(GL_TRIANGLES); /* compat immediate mode -- the proven canary shape */
    glVertex3f(-1.f, -1.f, 0.f);
    glVertex3f(3.f, -1.f, 0.f);
    glVertex3f(-1.f, 3.f, 0.f);
    glEnd();
}

} // namespace

int main() {
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        std::printf("MRT-CANARY: FAIL (no display)\n");
        return 2;
    }
    const int screen = DefaultScreen(dpy);
    int attrs[] = {GLX_X_RENDERABLE,
                   True,
                   GLX_RENDER_TYPE,
                   GLX_RGBA_BIT,
                   GLX_DOUBLEBUFFER,
                   True,
                   GLX_RED_SIZE,
                   8,
                   GLX_GREEN_SIZE,
                   8,
                   GLX_BLUE_SIZE,
                   8,
                   None};
    int n = 0;
    GLXFBConfig* fbc = glXChooseFBConfig(dpy, screen, attrs, &n);
    if (fbc == nullptr || n == 0) {
        std::printf("MRT-CANARY: FAIL (no fbconfig)\n");
        return 2;
    }
    XVisualInfo* vi = glXGetVisualFromFBConfig(dpy, fbc[0]);
    Colormap cmap = XCreateColormap(dpy, RootWindow(dpy, screen), vi->visual, AllocNone);
    XSetWindowAttributes swa;
    std::memset(&swa, 0, sizeof(swa));
    swa.colormap = cmap;
    Window win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, 64, 64, 0, vi->depth,
                               InputOutput, vi->visual, CWColormap, &swa);
    XMapWindow(dpy, win);
    XSync(dpy, False);
    PFNCCA cca = reinterpret_cast<PFNCCA>(gp("glXCreateContextAttribsARB"));
    GLXContext ctx = nullptr;
    if (cca != nullptr) {
        int cattrs[] = {0x2091, 3, 0x2092, 0, 0x9126, 0x2, None}; /* 3.0 COMPATIBILITY */
        ctx = cca(dpy, fbc[0], nullptr, True, cattrs);
    }
    if (ctx == nullptr) {
        ctx = glXCreateContext(dpy, vi, nullptr, True);
    }
    if (ctx == nullptr) {
        std::printf("MRT-CANARY: FAIL (no context)\n");
        return 2;
    }
    glXMakeCurrent(dpy, win, ctx);
    std::printf("MRT-CANARY: GL_VERSION=%s\n",
                reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    GLint maxdb = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS_C, &maxdb);
    std::printf("MRT-CANARY: MAX_DRAW_BUFFERS=%d\n", maxdb);
    if (maxdb < 3) {
        std::printf("MRT-CANARY: FAIL (needs >= 3 draw buffers)\n");
        return 2;
    }

    PFNGENFB gen_fb = reinterpret_cast<PFNGENFB>(gp("glGenFramebuffers"));
    PFNBINDFB bind_fb = reinterpret_cast<PFNBINDFB>(gp("glBindFramebuffer"));
    PFNGENRB gen_rb = reinterpret_cast<PFNGENRB>(gp("glGenRenderbuffers"));
    PFNBINDRB bind_rb = reinterpret_cast<PFNBINDRB>(gp("glBindRenderbuffer"));
    PFNRBSTORE rb_store = reinterpret_cast<PFNRBSTORE>(gp("glRenderbufferStorage"));
    PFNFBRB fb_rb = reinterpret_cast<PFNFBRB>(gp("glFramebufferRenderbuffer"));
    PFNCHECKFB check_fb = reinterpret_cast<PFNCHECKFB>(gp("glCheckFramebufferStatus"));
    PFNDRAWBUFS draw_bufs = reinterpret_cast<PFNDRAWBUFS>(gp("glDrawBuffers"));
    PFNCOLORMASKI color_maski = reinterpret_cast<PFNCOLORMASKI>(gp("glColorMaski"));
    PFNCREATESH create_sh = reinterpret_cast<PFNCREATESH>(gp("glCreateShader"));
    PFNSHSRC sh_src = reinterpret_cast<PFNSHSRC>(gp("glShaderSource"));
    PFNCOMPILE compile = reinterpret_cast<PFNCOMPILE>(gp("glCompileShader"));
    PFNGETSHIV get_shiv = reinterpret_cast<PFNGETSHIV>(gp("glGetShaderiv"));
    PFNCREATEPROG create_prog = reinterpret_cast<PFNCREATEPROG>(gp("glCreateProgram"));
    PFNATTACH attach = reinterpret_cast<PFNATTACH>(gp("glAttachShader"));
    PFNLINK link = reinterpret_cast<PFNLINK>(gp("glLinkProgram"));
    PFNGETPROGIV get_progiv = reinterpret_cast<PFNGETPROGIV>(gp("glGetProgramiv"));
    PFNUSEPROG use_prog = reinterpret_cast<PFNUSEPROG>(gp("glUseProgram"));
    if (gen_fb == nullptr || draw_bufs == nullptr || color_maski == nullptr ||
        create_sh == nullptr) {
        std::printf("MRT-CANARY: FAIL (entry points NULL)\n");
        return 2;
    }

    // A 3-attachment FBO (RGBA8 renderbuffers). Sub-gate A/B use 0+1; sub-gate C uses 0/gap/2.
    GLuint fbo = 0;
    GLuint rb[3] = {0, 0, 0};
    gen_fb(1, &fbo);
    bind_fb(GL_FRAMEBUFFER_C, fbo);
    gen_rb(3, rb);
    for (int i = 0; i < 3; ++i) {
        bind_rb(GL_RENDERBUFFER_C, rb[i]);
        rb_store(GL_RENDERBUFFER_C, GL_RGBA8, 64, 64);
        fb_rb(GL_FRAMEBUFFER_C, static_cast<GLenum>(GL_COLOR_ATTACHMENT0_C + i), GL_RENDERBUFFER_C,
              rb[i]);
    }
    const GLenum st = check_fb(GL_FRAMEBUFFER_C);
    std::printf("MRT-CANARY: fbo status=0x%x (%s)\n", st,
                st == GL_FRAMEBUFFER_COMPLETE_C ? "COMPLETE" : "NOT complete");
    if (st != GL_FRAMEBUFFER_COMPLETE_C) {
        std::printf("MRT-CANARY: FAIL (fbo incomplete)\n");
        return 1;
    }

    GLuint vs = create_sh(GL_VERTEX_SHADER_C);
    GLuint fs = create_sh(GL_FRAGMENT_SHADER_C);
    sh_src(vs, 1, &kVS, nullptr);
    compile(vs);
    sh_src(fs, 1, &kFS, nullptr);
    compile(fs);
    GLint ok = 0;
    get_shiv(fs, GL_COMPILE_STATUS_C, &ok);
    if (ok == 0) {
        std::printf("MRT-CANARY: FAIL (fragment shader compile)\n");
        return 1;
    }
    GLuint prog = create_prog();
    attach(prog, vs);
    attach(prog, fs);
    link(prog);
    get_progiv(prog, GL_LINK_STATUS_C, &ok);
    if (ok == 0) {
        std::printf("MRT-CANARY: FAIL (program link)\n");
        return 1;
    }
    use_prog(prog);
    glViewport(0, 0, 64, 64);

    const GLenum all3[3] = {GL_COLOR_ATTACHMENT0_C, GL_COLOR_ATTACHMENT0_C + 1,
                            GL_COLOR_ATTACHMENT0_C + 2};

    // Sub-gate A: 2-attachment distinct colors (the probe that silently half-rendered before).
    draw_bufs(3, all3); // clear ALL attachments to black first (full write masks)
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_bufs(2, all3);
    fullscreen_triangle();
    unsigned char p0[4];
    unsigned char p1[4];
    unsigned char p2[4];
    read_att(0, p0);
    read_att(1, p1);
    bool pass_a = px_is(p0, 0xCC, 0x33, 0x99, "A att0");
    pass_a = px_is(p1, 0x33, 0x99, 0xCC, "A att1") && pass_a;

    // Sub-gate B: a per-attachment write mask -- attachment 1's RED channel is masked off, so its
    // red keeps the black clear while G/B render (proves the blend-state ARRAY applies per
    // attachment through the relay, not just to attachment 0).
    draw_bufs(3, all3);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_bufs(2, all3);
    color_maski(1, GL_FALSE, GL_TRUE, GL_TRUE, GL_TRUE);
    fullscreen_triangle();
    color_maski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    read_att(0, p0);
    read_att(1, p1);
    bool pass_b = px_is(p0, 0xCC, 0x33, 0x99, "B att0");
    pass_b = px_is(p1, 0x00, 0x99, 0xCC, "B att1 (red masked)") && pass_b;

    // Sub-gate C: an UNUSED gap -- draw buffers {ATT0, NONE, ATT2}: zink emits a
    // VK_ATTACHMENT_UNUSED color-ref hole. The gap attachment keeps its clear color.
    draw_bufs(3, all3);
    glClear(GL_COLOR_BUFFER_BIT);
    const GLenum gapped[3] = {GL_COLOR_ATTACHMENT0_C, GL_NONE_C, GL_COLOR_ATTACHMENT0_C + 2};
    draw_bufs(3, gapped);
    fullscreen_triangle();
    read_att(0, p0);
    read_att(1, p1);
    read_att(2, p2);
    bool pass_c = px_is(p0, 0xCC, 0x33, 0x99, "C att0");
    pass_c = px_is(p1, 0x00, 0x00, 0x00, "C att1 (gap, untouched)") && pass_c;
    pass_c = px_is(p2, 0x99, 0xCC, 0x33, "C att2") && pass_c;

    const bool pass = pass_a && pass_b && pass_c;
    std::printf("MRT-CANARY: %s\n", pass ? "PASS" : "FAIL");
    glXMakeCurrent(dpy, None, nullptr);
    glXDestroyContext(dpy, ctx);
    XCloseDisplay(dpy);
    return pass ? 0 : 1;
}
