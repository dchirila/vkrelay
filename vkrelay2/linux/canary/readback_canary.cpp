// Offscreen-readback canary: proves GPU-written pixels cross the relay BACK to the guest
// (glReadPixels -- the OpenSCAD `-o png` export path). Opens a GLX compatibility context on the
// guest DISPLAY (the exact bring-up the in-repo query canary proved through the relay), then
// readpixels-checks two distinct single-color clears:
//   1. an app FBO (renderbuffer color attachment)   -> copy_image_to_buffer from an APP image;
//   2. the winsys back buffer (default framebuffer) -> copy_image_to_buffer from a SWAPCHAIN image
//      (exactly what the `-o png` export reads, per the readback notes).
// Self-judging: prints machine-checkable lines + PASS/FAIL; run_readback_smoke.sh gates on them.
// Pre-fix (RED) both reads return stale zeros (black): the staging memory zink copies the pixels
// into was never flagged for worker->ICD download, and zink's timeline-semaphore sync point never
// refreshed readbacks -- the app read the ICD shadow's zero-fill, not the GPU bytes.
#include <GL/gl.h>
#include <GL/glx.h>
#include <cstdio>
#include <cstring>

// Core-3.0 FBO entry points + enums (resolved at runtime via glXGetProcAddressARB; the system
// headers only prototype GL 1.x).
typedef void (*PFNGENFB)(GLsizei, GLuint*);
typedef void (*PFNBINDFB)(GLenum, GLuint);
typedef void (*PFNGENRB)(GLsizei, GLuint*);
typedef void (*PFNBINDRB)(GLenum, GLuint);
typedef void (*PFNRBSTORE)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (*PFNFBRB)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (*PFNCHECKFB)(GLenum);
typedef GLXContext (*PFNCCA)(Display*, GLXFBConfig, GLXContext, Bool, const int*);

#define GL_FRAMEBUFFER_C 0x8D40
#define GL_RENDERBUFFER_C 0x8D41
#define GL_COLOR_ATTACHMENT0_C 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE_C 0x8CD5

namespace {

void* gp(const char* n) {
    return reinterpret_cast<void*>(glXGetProcAddressARB(reinterpret_cast<const GLubyte*>(n)));
}

// One RGBA8 pixel as rrggbbaa hex (what the smoke greps).
void format_px(const unsigned char* p, char* out) {
    std::snprintf(out, 9, "%02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
}

} // namespace

int main() {
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        std::printf("READBACK-CANARY: FAIL (no display)\n");
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
        std::printf("READBACK-CANARY: FAIL (no fbconfig)\n");
        return 2;
    }
    XVisualInfo* vi = glXGetVisualFromFBConfig(dpy, fbc[0]);
    Colormap cmap = XCreateColormap(dpy, RootWindow(dpy, screen), vi->visual, AllocNone);
    XSetWindowAttributes swa;
    std::memset(&swa, 0, sizeof(swa));
    swa.colormap = cmap;
    swa.event_mask = StructureNotifyMask;
    Window win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, 64, 64, 0, vi->depth,
                               InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
    XMapWindow(dpy, win);
    XSync(dpy, False);

    PFNCCA cca = reinterpret_cast<PFNCCA>(gp("glXCreateContextAttribsARB"));
    GLXContext ctx = nullptr;
    if (cca != nullptr) {
        // Explicit COMPATIBILITY profile: the proven-through-relay context config (a Core request
        // changes zink's exposed paths; the export apps run compat). 0x9126 =
        // GLX_CONTEXT_PROFILE_MASK_ARB, 0x2 = COMPATIBILITY_PROFILE_BIT.
        int cattrs[] = {
            0x2091 /*MAJOR*/, 3, 0x2092 /*MINOR*/, 0, 0x9126 /*PROFILE_MASK*/, 0x2, None};
        ctx = cca(dpy, fbc[0], nullptr, True, cattrs);
    }
    if (ctx == nullptr) {
        ctx = glXCreateContext(dpy, vi, nullptr, True);
    }
    if (ctx == nullptr) {
        std::printf("READBACK-CANARY: FAIL (no context)\n");
        return 2;
    }
    glXMakeCurrent(dpy, win, ctx);
    std::printf("READBACK-CANARY: GL_VERSION=%s\n",
                reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    PFNGENFB gen_fb = reinterpret_cast<PFNGENFB>(gp("glGenFramebuffers"));
    PFNBINDFB bind_fb = reinterpret_cast<PFNBINDFB>(gp("glBindFramebuffer"));
    PFNGENRB gen_rb = reinterpret_cast<PFNGENRB>(gp("glGenRenderbuffers"));
    PFNBINDRB bind_rb = reinterpret_cast<PFNBINDRB>(gp("glBindRenderbuffer"));
    PFNRBSTORE rb_store = reinterpret_cast<PFNRBSTORE>(gp("glRenderbufferStorage"));
    PFNFBRB fb_rb = reinterpret_cast<PFNFBRB>(gp("glFramebufferRenderbuffer"));
    PFNCHECKFB check_fb = reinterpret_cast<PFNCHECKFB>(gp("glCheckFramebufferStatus"));
    if (gen_fb == nullptr || bind_fb == nullptr || gen_rb == nullptr || bind_rb == nullptr ||
        rb_store == nullptr || fb_rb == nullptr || check_fb == nullptr) {
        std::printf("READBACK-CANARY: FAIL (FBO entry points NULL)\n");
        return 2;
    }

    // 1. App-FBO readback: clear to (0.8, 0.2, 0.6, 1.0) -> exact RGBA8 cc3399ff.
    GLuint fbo = 0;
    GLuint rb = 0;
    gen_fb(1, &fbo);
    bind_fb(GL_FRAMEBUFFER_C, fbo);
    gen_rb(1, &rb);
    bind_rb(GL_RENDERBUFFER_C, rb);
    rb_store(GL_RENDERBUFFER_C, GL_RGBA8, 64, 64);
    fb_rb(GL_FRAMEBUFFER_C, GL_COLOR_ATTACHMENT0_C, GL_RENDERBUFFER_C, rb);
    const GLenum fb_status = check_fb(GL_FRAMEBUFFER_C);
    if (fb_status != GL_FRAMEBUFFER_COMPLETE_C) {
        std::printf("READBACK-CANARY: FAIL (FBO incomplete, status=0x%x)\n", fb_status);
        return 2;
    }
    glViewport(0, 0, 64, 64);
    glClearColor(0.8f, 0.2f, 0.6f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char fbo_edge[4] = {0, 0, 0, 0};
    unsigned char fbo_center[4] = {0, 0, 0, 0};
    glReadPixels(5, 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, fbo_edge);
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, fbo_center);
    char fbo_hex[9];
    format_px(fbo_center, fbo_hex);
    std::printf("READBACK-CANARY: fbo pixel=%s expected=cc3399ff\n", fbo_hex);
    const bool fbo_ok = fbo_center[0] == 0xCC && fbo_center[1] == 0x33 && fbo_center[2] == 0x99 &&
                        fbo_center[3] == 0xFF && std::memcmp(fbo_edge, fbo_center, 4) == 0;

    // 2. Winsys (default framebuffer, back buffer) readback: clear to (0.2, 0.6, 0.8, 1.0) ->
    // 3399cc. RGB-only compare (a winsys visual without alpha bits reads back alpha = 0xFF, but
    // that is the visual's business, not the readback path's).
    bind_fb(GL_FRAMEBUFFER_C, 0);
    glReadBuffer(GL_BACK); // explicit: no reliance on the double-buffered default
    glClearColor(0.2f, 0.6f, 0.8f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char win_px[4] = {0, 0, 0, 0};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, win_px);
    std::printf("READBACK-CANARY: winsys pixel=%02x%02x%02x expected=3399cc\n", win_px[0],
                win_px[1], win_px[2]);
    const bool win_ok = win_px[0] == 0x33 && win_px[1] == 0x99 && win_px[2] == 0xCC;

    std::printf("READBACK-CANARY: %s\n", (fbo_ok && win_ok)
                                             ? "PASS"
                                             : (fbo_ok ? "FAIL (winsys readback wrong)"
                                                       : (win_ok ? "FAIL (fbo readback wrong)"
                                                                 : "FAIL (both readbacks wrong)")));
    glXMakeCurrent(dpy, None, nullptr);
    glXDestroyContext(dpy, ctx);
    XCloseDisplay(dpy);
    return (fbo_ok && win_ok) ? 0 : 1;
}
