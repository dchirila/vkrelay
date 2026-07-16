// Tessellation + geometry-shader canary: the features are advertised TRUE
// (the honest host features carry tessellationShader/geometryShader), but no gate ever ran the
// paths -- zink emits TESC/TESE/GEOM SPIR-V stages and PATCH_LIST pipelines the relay had never
// seen. Two self-judging sub-gates on one GL 4.0 CORE context (zink), each designed so the
// stage RUNNING is what the pixels prove (not just "something drew"):
//
//   A. TESSELLATION: one fullscreen-triangle PATCH (GL_PATCHES, patch size 3); the TES SHRINKS
//      every tessellated vertex toward the patch centroid by 0.5. If the TCS/TES actually ran,
//      the drawn area is the CENTERED half-size triangle: center pixel = the tess color AND the
//      corner pixel stays background. A tess-skipped draw of the raw patch would paint the
//      corner too -- the shrink is the witness.
//   B. GEOMETRY: one POINT; the GS expands it into a centered quad (+-0.5 NDC,
//      triangle_strip max_vertices=4). A bare point can paint ~one pixel; the GS quad paints
//      two far-apart interior probes while the corner stays background -- the expansion is the
//      witness.
//
// Rendered into an RGBA8 FBO, read back with glReadPixels (the readback contract).
// Greppable TESSGEOM-CANARY markers; PASS + exit 0 only when both sub-gates hold exactly.
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
typedef GLuint (*PFNCREATESH)(GLenum);
typedef void (*PFNSHSRC)(GLuint, GLsizei, const char* const*, const GLint*);
typedef void (*PFNCOMPILE)(GLuint);
typedef void (*PFNGETSHIV)(GLuint, GLenum, GLint*);
typedef void (*PFNGETSHLOG)(GLuint, GLsizei, GLsizei*, char*);
typedef GLuint (*PFNCREATEPROG)(void);
typedef void (*PFNATTACH)(GLuint, GLuint);
typedef void (*PFNLINK)(GLuint);
typedef void (*PFNGETPROGIV)(GLuint, GLenum, GLint*);
typedef void (*PFNUSEPROG)(GLuint);
typedef void (*PFNGENVA)(GLsizei, GLuint*);
typedef void (*PFNBINDVA)(GLuint);
typedef void (*PFNGENBUF)(GLsizei, GLuint*);
typedef void (*PFNBINDBUF)(GLenum, GLuint);
typedef void (*PFNBUFDATA)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void (*PFNVAP)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void (*PFNEVA)(GLuint);
typedef void (*PFNPATCHPARAM)(GLenum, GLint);

#define GL_FRAMEBUFFER_C 0x8D40
#define GL_RENDERBUFFER_C 0x8D41
#define GL_COLOR_ATTACHMENT0_C 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE_C 0x8CD5
#define GL_FRAGMENT_SHADER_C 0x8B30
#define GL_VERTEX_SHADER_C 0x8B31
#define GL_GEOMETRY_SHADER_C 0x8DD9
#define GL_TESS_CONTROL_SHADER_C 0x8E88
#define GL_TESS_EVALUATION_SHADER_C 0x8E87
#define GL_COMPILE_STATUS_C 0x8B81
#define GL_LINK_STATUS_C 0x8B82
#define GL_ARRAY_BUFFER_C 0x8892
#define GL_STATIC_DRAW_C 0x88E4
#define GL_PATCHES_C 0x000E
#define GL_PATCH_VERTICES_C 0x8E72

namespace {

void* gp(const char* n) {
    return reinterpret_cast<void*>(glXGetProcAddressARB(reinterpret_cast<const GLubyte*>(n)));
}

// --- Sub-gate A: tessellation (GLSL 400 core) -- the TES shrinks toward the centroid. ---
const char* kVsPass = "#version 400 core\n"
                      "layout(location = 0) in vec2 pos;\n"
                      "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
const char* kTcs =
    "#version 400 core\n"
    "layout(vertices = 3) out;\n"
    "void main() {\n"
    "  gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
    "  if (gl_InvocationID == 0) {\n"
    "    gl_TessLevelOuter[0] = 4.0; gl_TessLevelOuter[1] = 4.0; gl_TessLevelOuter[2] = 4.0;\n"
    "    gl_TessLevelInner[0] = 4.0;\n"
    "  }\n"
    "}\n";
const char* kTes =
    "#version 400 core\n"
    "layout(triangles, equal_spacing, ccw) in;\n"
    "void main() {\n"
    "  vec4 p = gl_TessCoord.x * gl_in[0].gl_Position + gl_TessCoord.y * gl_in[1].gl_Position +\n"
    "           gl_TessCoord.z * gl_in[2].gl_Position;\n"
    "  vec4 c = (gl_in[0].gl_Position + gl_in[1].gl_Position + gl_in[2].gl_Position) / 3.0;\n"
    "  gl_Position = vec4(mix(c.xy, p.xy, 0.5), 0.0, 1.0);\n" /* SHRINK: the tess witness */
    "}\n";
const char* kFsTess = "#version 400 core\n"
                      "out vec4 color;\n"
                      "void main() { color = vec4(0.8, 0.2, 0.6, 1.0); }\n"; /* cc3399 */

// --- Sub-gate B: geometry (GLSL 400 core) -- one point becomes a centered quad. ---
const char* kGs = "#version 400 core\n"
                  "layout(points) in;\n"
                  "layout(triangle_strip, max_vertices = 4) out;\n"
                  "void main() {\n"
                  "  vec4 c = gl_in[0].gl_Position;\n"
                  "  gl_Position = c + vec4(-0.5, -0.5, 0.0, 0.0); EmitVertex();\n"
                  "  gl_Position = c + vec4(0.5, -0.5, 0.0, 0.0); EmitVertex();\n"
                  "  gl_Position = c + vec4(-0.5, 0.5, 0.0, 0.0); EmitVertex();\n"
                  "  gl_Position = c + vec4(0.5, 0.5, 0.0, 0.0); EmitVertex();\n"
                  "  EndPrimitive();\n"
                  "}\n";
const char* kFsGeom = "#version 400 core\n"
                      "out vec4 color;\n"
                      "void main() { color = vec4(0.2, 0.6, 0.8, 1.0); }\n"; /* 3399cc */

PFNGETSHIV g_get_shiv = nullptr;
PFNGETSHLOG g_get_shlog = nullptr;

GLuint make_shader(PFNCREATESH create_sh, PFNSHSRC sh_src, PFNCOMPILE compile, GLenum kind,
                   const char* src, const char* what) {
    const GLuint s = create_sh(kind);
    sh_src(s, 1, &src, nullptr);
    compile(s);
    GLint ok = 0;
    g_get_shiv(s, GL_COMPILE_STATUS_C, &ok);
    if (ok == 0) {
        char log[512] = {0};
        g_get_shlog(s, sizeof(log) - 1, nullptr, log);
        std::printf("TESSGEOM-CANARY: FAIL (%s shader compile: %s)\n", what, log);
        return 0;
    }
    return s;
}

bool px_is(int x, int y, unsigned r, unsigned g, unsigned b, const char* what) {
    unsigned char px[4] = {0, 0, 0, 0};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    const bool ok = px[0] == r && px[1] == g && px[2] == b;
    std::printf("TESSGEOM-CANARY: %s (%d,%d) pixel=%02x%02x%02x expected=%02x%02x%02x %s\n", what,
                x, y, px[0], px[1], px[2], r, g, b, ok ? "OK" : "WRONG");
    return ok;
}

} // namespace

int main() {
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        std::printf("TESSGEOM-CANARY: FAIL (no display)\n");
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
        std::printf("TESSGEOM-CANARY: FAIL (no fbconfig)\n");
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
        int cattrs[] = {0x2091, 4, 0x2092, 0, 0x9126, 0x1, None}; /* 4.0 CORE (tess minimum) */
        ctx = cca(dpy, fbc[0], nullptr, True, cattrs);
    }
    if (ctx == nullptr) {
        std::printf("TESSGEOM-CANARY: FAIL (no GL 4.0 core context)\n");
        return 1;
    }
    glXMakeCurrent(dpy, win, ctx);
    std::printf("TESSGEOM-CANARY: GL_VERSION=%s\n",
                reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    // The relay-honesty check: the smoke asserts this names zink (the GL->Vulkan frontend over
    // OUR ICD), never a software fallback.
    std::printf("TESSGEOM-CANARY: GL_RENDERER=%s\n",
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    PFNGENFB gen_fb = reinterpret_cast<PFNGENFB>(gp("glGenFramebuffers"));
    PFNBINDFB bind_fb = reinterpret_cast<PFNBINDFB>(gp("glBindFramebuffer"));
    PFNGENRB gen_rb = reinterpret_cast<PFNGENRB>(gp("glGenRenderbuffers"));
    PFNBINDRB bind_rb = reinterpret_cast<PFNBINDRB>(gp("glBindRenderbuffer"));
    PFNRBSTORE rb_store = reinterpret_cast<PFNRBSTORE>(gp("glRenderbufferStorage"));
    PFNFBRB fb_rb = reinterpret_cast<PFNFBRB>(gp("glFramebufferRenderbuffer"));
    PFNCHECKFB check_fb = reinterpret_cast<PFNCHECKFB>(gp("glCheckFramebufferStatus"));
    PFNCREATESH create_sh = reinterpret_cast<PFNCREATESH>(gp("glCreateShader"));
    PFNSHSRC sh_src = reinterpret_cast<PFNSHSRC>(gp("glShaderSource"));
    PFNCOMPILE compile = reinterpret_cast<PFNCOMPILE>(gp("glCompileShader"));
    g_get_shiv = reinterpret_cast<PFNGETSHIV>(gp("glGetShaderiv"));
    g_get_shlog = reinterpret_cast<PFNGETSHLOG>(gp("glGetShaderInfoLog"));
    PFNCREATEPROG create_prog = reinterpret_cast<PFNCREATEPROG>(gp("glCreateProgram"));
    PFNATTACH attach = reinterpret_cast<PFNATTACH>(gp("glAttachShader"));
    PFNLINK link = reinterpret_cast<PFNLINK>(gp("glLinkProgram"));
    PFNGETPROGIV get_progiv = reinterpret_cast<PFNGETPROGIV>(gp("glGetProgramiv"));
    PFNUSEPROG use_prog = reinterpret_cast<PFNUSEPROG>(gp("glUseProgram"));
    PFNGENVA gen_va = reinterpret_cast<PFNGENVA>(gp("glGenVertexArrays"));
    PFNBINDVA bind_va = reinterpret_cast<PFNBINDVA>(gp("glBindVertexArray"));
    PFNGENBUF gen_buf = reinterpret_cast<PFNGENBUF>(gp("glGenBuffers"));
    PFNBINDBUF bind_buf = reinterpret_cast<PFNBINDBUF>(gp("glBindBuffer"));
    PFNBUFDATA buf_data = reinterpret_cast<PFNBUFDATA>(gp("glBufferData"));
    PFNVAP vap = reinterpret_cast<PFNVAP>(gp("glVertexAttribPointer"));
    PFNEVA eva = reinterpret_cast<PFNEVA>(gp("glEnableVertexAttribArray"));
    PFNPATCHPARAM patch_param = reinterpret_cast<PFNPATCHPARAM>(gp("glPatchParameteri"));
    // Hardening: check EVERY loaded pointer, so an environment failure reports
    // cleanly instead of becoming a null-call crash.
    if (gen_fb == nullptr || bind_fb == nullptr || gen_rb == nullptr || bind_rb == nullptr ||
        rb_store == nullptr || fb_rb == nullptr || check_fb == nullptr || create_sh == nullptr ||
        sh_src == nullptr || compile == nullptr || g_get_shiv == nullptr ||
        g_get_shlog == nullptr || create_prog == nullptr || attach == nullptr || link == nullptr ||
        get_progiv == nullptr || use_prog == nullptr || gen_va == nullptr || bind_va == nullptr ||
        gen_buf == nullptr || bind_buf == nullptr || buf_data == nullptr || vap == nullptr ||
        eva == nullptr || patch_param == nullptr) {
        std::printf("TESSGEOM-CANARY: FAIL (entry points NULL)\n");
        return 1;
    }

    // RGBA8 FBO, 64x64.
    GLuint fbo = 0;
    GLuint rb = 0;
    gen_fb(1, &fbo);
    bind_fb(GL_FRAMEBUFFER_C, fbo);
    gen_rb(1, &rb);
    bind_rb(GL_RENDERBUFFER_C, rb);
    rb_store(GL_RENDERBUFFER_C, GL_RGBA8, 64, 64);
    fb_rb(GL_FRAMEBUFFER_C, GL_COLOR_ATTACHMENT0_C, GL_RENDERBUFFER_C, rb);
    if (check_fb(GL_FRAMEBUFFER_C) != GL_FRAMEBUFFER_COMPLETE_C) {
        std::printf("TESSGEOM-CANARY: FAIL (fbo incomplete)\n");
        return 1;
    }
    glViewport(0, 0, 64, 64);

    // Shared geometry: the fullscreen triangle (patch input) and one centered point.
    const float tri[6] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};
    const float pt[2] = {0.f, 0.f};
    GLuint va[2] = {0, 0};
    GLuint vb[2] = {0, 0};
    gen_va(2, va);
    gen_buf(2, vb);
    bind_va(va[0]);
    bind_buf(GL_ARRAY_BUFFER_C, vb[0]);
    buf_data(GL_ARRAY_BUFFER_C, sizeof(tri), tri, GL_STATIC_DRAW_C);
    vap(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    eva(0);
    bind_va(va[1]);
    bind_buf(GL_ARRAY_BUFFER_C, vb[1]);
    buf_data(GL_ARRAY_BUFFER_C, sizeof(pt), pt, GL_STATIC_DRAW_C);
    vap(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    eva(0);

    auto link_ok = [&](GLuint prog, const char* what) {
        link(prog);
        GLint ok = 0;
        get_progiv(prog, GL_LINK_STATUS_C, &ok);
        if (ok == 0) {
            std::printf("TESSGEOM-CANARY: FAIL (%s link)\n", what);
            return false;
        }
        return true;
    };

    bool pass = true;

    // --- Sub-gate A: tessellation ---
    {
        const GLuint vs =
            make_shader(create_sh, sh_src, compile, GL_VERTEX_SHADER_C, kVsPass, "tess vertex");
        const GLuint tcs =
            make_shader(create_sh, sh_src, compile, GL_TESS_CONTROL_SHADER_C, kTcs, "tess control");
        const GLuint tes = make_shader(create_sh, sh_src, compile, GL_TESS_EVALUATION_SHADER_C,
                                       kTes, "tess evaluation");
        const GLuint fs =
            make_shader(create_sh, sh_src, compile, GL_FRAGMENT_SHADER_C, kFsTess, "tess fragment");
        if (vs == 0 || tcs == 0 || tes == 0 || fs == 0) {
            return 1;
        }
        const GLuint prog = create_prog();
        attach(prog, vs);
        attach(prog, tcs);
        attach(prog, tes);
        attach(prog, fs);
        if (!link_ok(prog, "tess program")) {
            return 1;
        }
        use_prog(prog);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        bind_va(va[0]);
        patch_param(GL_PATCH_VERTICES_C, 3);
        glDrawArrays(GL_PATCHES_C, 0, 3);
        glFinish();
        // The shrunk triangle covers the center but NOT the corner: both are the witness.
        pass &= px_is(32, 32, 0xcc, 0x33, 0x99, "tess center");
        pass &= px_is(2, 2, 0x00, 0x00, 0x00, "tess corner (must stay background)");
    }

    // --- Sub-gate B: geometry ---
    {
        const GLuint vs =
            make_shader(create_sh, sh_src, compile, GL_VERTEX_SHADER_C, kVsPass, "geom vertex");
        const GLuint gs =
            make_shader(create_sh, sh_src, compile, GL_GEOMETRY_SHADER_C, kGs, "geometry");
        const GLuint fs =
            make_shader(create_sh, sh_src, compile, GL_FRAGMENT_SHADER_C, kFsGeom, "geom fragment");
        if (vs == 0 || gs == 0 || fs == 0) {
            return 1;
        }
        const GLuint prog = create_prog();
        attach(prog, vs);
        attach(prog, gs);
        attach(prog, fs);
        if (!link_ok(prog, "geom program")) {
            return 1;
        }
        use_prog(prog);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        bind_va(va[1]);
        glDrawArrays(GL_POINTS, 0, 1);
        glFinish();
        // The GS quad covers (16,16)..(48,48): two far-apart interior probes + a clean corner.
        pass &= px_is(20, 20, 0x33, 0x99, 0xcc, "geom quad low-left probe");
        pass &= px_is(44, 44, 0x33, 0x99, 0xcc, "geom quad high-right probe");
        pass &= px_is(2, 2, 0x00, 0x00, 0x00, "geom corner (must stay background)");
    }

    if (!pass) {
        std::printf("TESSGEOM-CANARY: FAIL (a sub-gate rendered wrong)\n");
        return 1;
    }
    std::printf("TESSGEOM-CANARY: PASS\n");
    return 0;
}
