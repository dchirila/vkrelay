// AMD-iGPU viewport-corruption diagnostic: a self-judging, multi-frame GL/zink ladder.
//
// The affected real application renders its first observed frame correctly, then corrupts the
// 3D viewport while 2D chrome stays crisp. This canary targets that transition through the same
// GL -> zink -> vkrelay2 path, but replaces visual judgment with exact pixel probes. One binary
// keeps the rungs comparable:
//
//   clear-reuse    Reuse one color+depth FBO and alternate full clears for 16 frames.
//   scissor-clear  Per frame, full-clear then scissored-clear (zink's known loadOp versus
//                  vkCmdClearAttachments transition class).
//   attachment-rotate  Rebind alternating color+depth views under one GL FBO every frame. Zink
//                  represents this class with an imageless VkFramebuffer plus per-begin views.
//   vbo-update     Replace a VBO every frame so a triangle alternates over/away from the center.
//   persistent-map Rewrite one triangle of a persistently mapped quad, comparing coherent
//                  in-place, explicit-flush in-place, and fresh-offset ring writes.
//
// Usage: vkrelay2-frame-transition-canary [--rung all|clear-reuse|scissor-clear|vbo-update]
// Greppable FRAME-TRANSITION-CANARY lines report every frame. A correct frame 0 is a precondition,
// not success: every later frame must also match.
#include <GL/gl.h>
#include <GL/glx.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr int kDim = 64;
constexpr int kFrames = 16;

constexpr GLenum kFramebuffer = 0x8D40;
constexpr GLenum kRenderbuffer = 0x8D41;
constexpr GLenum kColorAttachment0 = 0x8CE0;
constexpr GLenum kDepthAttachment = 0x8D00;
constexpr GLenum kFramebufferComplete = 0x8CD5;
constexpr GLenum kDepthComponent24 = 0x81A6;
constexpr GLenum kArrayBuffer = 0x8892;
constexpr GLenum kDynamicDraw = 0x88E8;
constexpr GLenum kMapWriteBit = 0x0002;
constexpr GLenum kMapFlushExplicitBit = 0x0010;
constexpr GLenum kMapPersistentBit = 0x0040;
constexpr GLenum kMapCoherentBit = 0x0080;
constexpr GLenum kVertexShader = 0x8B31;
constexpr GLenum kFragmentShader = 0x8B30;
constexpr GLenum kCompileStatus = 0x8B81;
constexpr GLenum kLinkStatus = 0x8B82;

using CreateContextAttribs = GLXContext (*)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
using GenFramebuffers = void (*)(GLsizei, GLuint*);
using BindFramebuffer = void (*)(GLenum, GLuint);
using GenRenderbuffers = void (*)(GLsizei, GLuint*);
using BindRenderbuffer = void (*)(GLenum, GLuint);
using RenderbufferStorage = void (*)(GLenum, GLenum, GLsizei, GLsizei);
using FramebufferRenderbuffer = void (*)(GLenum, GLenum, GLenum, GLuint);
using CheckFramebufferStatus = GLenum (*)(GLenum);
using CreateShader = GLuint (*)(GLenum);
using ShaderSource = void (*)(GLuint, GLsizei, const char* const*, const GLint*);
using CompileShader = void (*)(GLuint);
using GetShaderiv = void (*)(GLuint, GLenum, GLint*);
using GetShaderInfoLog = void (*)(GLuint, GLsizei, GLsizei*, char*);
using CreateProgram = GLuint (*)();
using AttachShader = void (*)(GLuint, GLuint);
using BindAttribLocation = void (*)(GLuint, GLuint, const char*);
using LinkProgram = void (*)(GLuint);
using GetProgramiv = void (*)(GLuint, GLenum, GLint*);
using GetProgramInfoLog = void (*)(GLuint, GLsizei, GLsizei*, char*);
using UseProgram = void (*)(GLuint);
using GenVertexArrays = void (*)(GLsizei, GLuint*);
using BindVertexArray = void (*)(GLuint);
using GenBuffers = void (*)(GLsizei, GLuint*);
using BindBuffer = void (*)(GLenum, GLuint);
using BufferData = void (*)(GLenum, GLsizeiptr, const void*, GLenum);
using BufferSubData = void (*)(GLenum, GLintptr, GLsizeiptr, const void*);
using BufferStorage = void (*)(GLenum, GLsizeiptr, const void*, GLbitfield);
using MapBufferRange = void* (*) (GLenum, GLintptr, GLsizeiptr, GLbitfield);
using FlushMappedBufferRange = void (*)(GLenum, GLintptr, GLsizeiptr);
using VertexAttribPointer = void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using EnableVertexAttribArray = void (*)(GLuint);

struct Gl {
    GenFramebuffers gen_framebuffers = nullptr;
    BindFramebuffer bind_framebuffer = nullptr;
    GenRenderbuffers gen_renderbuffers = nullptr;
    BindRenderbuffer bind_renderbuffer = nullptr;
    RenderbufferStorage renderbuffer_storage = nullptr;
    FramebufferRenderbuffer framebuffer_renderbuffer = nullptr;
    CheckFramebufferStatus check_framebuffer_status = nullptr;
    CreateShader create_shader = nullptr;
    ShaderSource shader_source = nullptr;
    CompileShader compile_shader = nullptr;
    GetShaderiv get_shader_iv = nullptr;
    GetShaderInfoLog get_shader_log = nullptr;
    CreateProgram create_program = nullptr;
    AttachShader attach_shader = nullptr;
    BindAttribLocation bind_attrib_location = nullptr;
    LinkProgram link_program = nullptr;
    GetProgramiv get_program_iv = nullptr;
    GetProgramInfoLog get_program_log = nullptr;
    UseProgram use_program = nullptr;
    GenVertexArrays gen_vertex_arrays = nullptr;
    BindVertexArray bind_vertex_array = nullptr;
    GenBuffers gen_buffers = nullptr;
    BindBuffer bind_buffer = nullptr;
    BufferData buffer_data = nullptr;
    BufferSubData buffer_sub_data = nullptr;
    BufferStorage buffer_storage = nullptr;
    MapBufferRange map_buffer_range = nullptr;
    FlushMappedBufferRange flush_mapped_buffer_range = nullptr;
    VertexAttribPointer vertex_attrib_pointer = nullptr;
    EnableVertexAttribArray enable_vertex_attrib_array = nullptr;
};

void* gp(const char* name) {
    return reinterpret_cast<void*>(glXGetProcAddressARB(reinterpret_cast<const GLubyte*>(name)));
}

template <typename T> bool load(T& fn, const char* name) {
    fn = reinterpret_cast<T>(gp(name));
    if (fn == nullptr) {
        std::printf("FRAME-TRANSITION-CANARY: FAIL (entry point %s is NULL)\n", name);
        return false;
    }
    return true;
}

bool load_gl(Gl& g) {
    return load(g.gen_framebuffers, "glGenFramebuffers") &&
           load(g.bind_framebuffer, "glBindFramebuffer") &&
           load(g.gen_renderbuffers, "glGenRenderbuffers") &&
           load(g.bind_renderbuffer, "glBindRenderbuffer") &&
           load(g.renderbuffer_storage, "glRenderbufferStorage") &&
           load(g.framebuffer_renderbuffer, "glFramebufferRenderbuffer") &&
           load(g.check_framebuffer_status, "glCheckFramebufferStatus") &&
           load(g.create_shader, "glCreateShader") && load(g.shader_source, "glShaderSource") &&
           load(g.compile_shader, "glCompileShader") && load(g.get_shader_iv, "glGetShaderiv") &&
           load(g.get_shader_log, "glGetShaderInfoLog") &&
           load(g.create_program, "glCreateProgram") && load(g.attach_shader, "glAttachShader") &&
           load(g.bind_attrib_location, "glBindAttribLocation") &&
           load(g.link_program, "glLinkProgram") && load(g.get_program_iv, "glGetProgramiv") &&
           load(g.get_program_log, "glGetProgramInfoLog") && load(g.use_program, "glUseProgram") &&
           load(g.gen_vertex_arrays, "glGenVertexArrays") &&
           load(g.bind_vertex_array, "glBindVertexArray") && load(g.gen_buffers, "glGenBuffers") &&
           load(g.bind_buffer, "glBindBuffer") && load(g.buffer_data, "glBufferData") &&
           load(g.buffer_sub_data, "glBufferSubData") &&
           load(g.buffer_storage, "glBufferStorage") &&
           load(g.map_buffer_range, "glMapBufferRange") &&
           load(g.flush_mapped_buffer_range, "glFlushMappedBufferRange") &&
           load(g.vertex_attrib_pointer, "glVertexAttribPointer") &&
           load(g.enable_vertex_attrib_array, "glEnableVertexAttribArray");
}

bool pixel_near(const unsigned char (&px)[4], unsigned r, unsigned g, unsigned b) {
    auto near = [](unsigned char actual, unsigned expected) {
        const int d = static_cast<int>(actual) - static_cast<int>(expected);
        return d >= -1 && d <= 1;
    };
    return near(px[0], r) && near(px[1], g) && near(px[2], b) && px[3] >= 0xFE;
}

bool probe(const char* rung, int frame, const char* point, int x, int y, unsigned r, unsigned g,
           unsigned b) {
    unsigned char px[4] = {0, 0, 0, 0};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    const bool ok = pixel_near(px, r, g, b);
    std::printf("FRAME-TRANSITION-CANARY: rung=%s frame=%d point=%s "
                "pixel=%02x%02x%02x%02x expected=%02x%02x%02xff %s\n",
                rung, frame, point, px[0], px[1], px[2], px[3], r, g, b, ok ? "OK" : "WRONG");
    return ok;
}

bool run_clear_reuse() {
    bool pass = true;
    for (int frame = 0; frame < kFrames; ++frame) {
        const bool blue = (frame % 2) == 0;
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, blue ? 0.0f : 1.0f, blue ? 1.0f : 0.0f, 1.0f);
        glClearDepth(blue ? 1.0 : 0.25);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glFinish();
        const bool frame_ok = blue ? probe("clear-reuse", frame, "center", 32, 32, 0, 0, 255)
                                   : probe("clear-reuse", frame, "center", 32, 32, 0, 255, 0);
        pass = frame_ok && pass;
    }
    return pass;
}

bool run_scissor_clear() {
    bool pass = true;
    for (int frame = 0; frame < kFrames; ++frame) {
        const bool left = (frame % 2) == 0;
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); // eligible for a loadOp clear

        glEnable(GL_SCISSOR_TEST);
        glScissor(left ? 0 : kDim / 2, 0, kDim / 2, kDim);
        glClearColor(left ? 1.0f : 0.0f, left ? 0.0f : 1.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT); // in-pass scissored clear / vkCmdClearAttachments class
        glDisable(GL_SCISSOR_TEST);
        glFinish();

        bool frame_ok = left ? probe("scissor-clear", frame, "left", 16, 32, 255, 0, 0)
                             : probe("scissor-clear", frame, "left", 16, 32, 0, 0, 0);
        frame_ok = (left ? probe("scissor-clear", frame, "right", 48, 32, 0, 0, 0)
                         : probe("scissor-clear", frame, "right", 48, 32, 0, 255, 0)) &&
                   frame_ok;
        pass = frame_ok && pass;
    }
    return pass;
}

bool run_attachment_rotation(const Gl& g, GLuint (&colors)[2], GLuint (&depths)[2]) {
    bool pass = true;
    for (int frame = 0; frame < kFrames; ++frame) {
        const int slot = frame % 2;
        g.framebuffer_renderbuffer(kFramebuffer, kColorAttachment0, kRenderbuffer, colors[slot]);
        g.framebuffer_renderbuffer(kFramebuffer, kDepthAttachment, kRenderbuffer, depths[slot]);
        if (g.check_framebuffer_status(kFramebuffer) != kFramebufferComplete) {
            std::printf("FRAME-TRANSITION-CANARY: rung=attachment-rotate frame=%d "
                        "FAIL (FBO incomplete)\n",
                        frame);
            return false;
        }

        glDisable(GL_SCISSOR_TEST);
        glClearColor(slot == 0 ? 1.0f : 0.0f, 0.0f, slot == 0 ? 0.0f : 1.0f, 1.0f);
        glClearDepth(slot == 0 ? 1.0 : 0.25);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glFinish();
        const bool frame_ok = slot == 0
                                  ? probe("attachment-rotate", frame, "center", 32, 32, 255, 0, 0)
                                  : probe("attachment-rotate", frame, "center", 32, 32, 0, 0, 255);
        pass = frame_ok && pass;
    }

    // Leave the FBO in the same complete state used by the other rungs.
    g.framebuffer_renderbuffer(kFramebuffer, kColorAttachment0, kRenderbuffer, colors[0]);
    g.framebuffer_renderbuffer(kFramebuffer, kDepthAttachment, kRenderbuffer, depths[0]);
    return pass;
}

GLuint make_shader(const Gl& g, GLenum kind, const char* source, const char* label) {
    const GLuint shader = g.create_shader(kind);
    g.shader_source(shader, 1, &source, nullptr);
    g.compile_shader(shader);
    GLint ok = 0;
    g.get_shader_iv(shader, kCompileStatus, &ok);
    if (ok == 0) {
        char log[1024] = {0};
        g.get_shader_log(shader, sizeof(log) - 1, nullptr, log);
        std::printf("FRAME-TRANSITION-CANARY: FAIL (%s shader compile: %s)\n", label, log);
        return 0;
    }
    return shader;
}

bool run_vbo_update(const Gl& g) {
    const char* vs_source = "#version 120\n"
                            "attribute vec2 inPos; attribute vec3 inColor; varying vec3 color;\n"
                            "void main() { gl_Position=vec4(inPos,0.0,1.0); color=inColor; }\n";
    const char* fs_source = "#version 120\n"
                            "varying vec3 color; void main() { gl_FragColor=vec4(color,1.0); }\n";
    const GLuint vs = make_shader(g, kVertexShader, vs_source, "vertex");
    const GLuint fs = make_shader(g, kFragmentShader, fs_source, "fragment");
    if (vs == 0 || fs == 0) {
        return false;
    }
    const GLuint program = g.create_program();
    g.attach_shader(program, vs);
    g.attach_shader(program, fs);
    g.bind_attrib_location(program, 0, "inPos");
    g.bind_attrib_location(program, 1, "inColor");
    g.link_program(program);
    GLint linked = 0;
    g.get_program_iv(program, kLinkStatus, &linked);
    if (linked == 0) {
        char log[1024] = {0};
        g.get_program_log(program, sizeof(log) - 1, nullptr, log);
        std::printf("FRAME-TRANSITION-CANARY: FAIL (program link: %s)\n", log);
        return false;
    }

    GLuint vao = 0;
    GLuint vbo = 0;
    g.gen_vertex_arrays(1, &vao);
    g.bind_vertex_array(vao);
    g.gen_buffers(1, &vbo);
    g.bind_buffer(kArrayBuffer, vbo);
    g.buffer_data(kArrayBuffer, 15 * sizeof(float), nullptr, kDynamicDraw);
    g.vertex_attrib_pointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
    g.enable_vertex_attrib_array(0);
    g.vertex_attrib_pointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                            reinterpret_cast<const void*>(2 * sizeof(float)));
    g.enable_vertex_attrib_array(1);
    g.use_program(program);

    constexpr float centered[15] = {
        -0.7f, -0.7f, 1.0f, 0.0f, 0.0f, 0.7f, -0.7f, 1.0f, 0.0f, 0.0f, 0.0f, 0.7f, 1.0f, 0.0f, 0.0f,
    };
    constexpr float shifted[15] = {
        0.55f, -0.3f, 1.0f,  0.0f, 0.0f, 0.95f, -0.3f, 1.0f,
        0.0f,  0.0f,  0.75f, 0.3f, 1.0f, 0.0f,  0.0f,
    };

    bool pass = true;
    for (int frame = 0; frame < kFrames; ++frame) {
        const bool centered_frame = (frame % 2) == 0;
        const float* vertices = centered_frame ? centered : shifted;
        g.buffer_sub_data(kArrayBuffer, 0, sizeof(centered), vertices);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
        const bool frame_ok = centered_frame
                                  ? probe("vbo-update", frame, "center", 32, 32, 255, 0, 0)
                                  : probe("vbo-update", frame, "center", 32, 32, 0, 0, 255);
        pass = frame_ok && pass;
    }
    g.use_program(0);
    return pass;
}

struct QuadVertex {
    float x;
    float y;
    float r;
    float g;
    float b;
};

enum class PersistentVariant { CoherentInPlace, ExplicitFlush, FreshOffset, FreshOffsetPadded };

const char* variant_name(PersistentVariant variant) {
    switch (variant) {
    case PersistentVariant::CoherentInPlace:
        return "coherent-in-place";
    case PersistentVariant::ExplicitFlush:
        return "explicit-flush";
    case PersistentVariant::FreshOffset:
        return "fresh-offset";
    case PersistentVariant::FreshOffsetPadded:
        return "fresh-offset-padded";
    }
    return "unknown";
}

bool run_persistent_variant(const Gl& g, GLuint program, GLuint vao, PersistentVariant variant) {
    constexpr std::size_t kVerticesPerQuad = 6;
    constexpr std::size_t kSlots = kFrames;
    constexpr std::size_t kQuadBytes = kVerticesPerQuad * sizeof(QuadVertex);
    const bool fresh_offset = variant == PersistentVariant::FreshOffset ||
                              variant == PersistentVariant::FreshOffsetPadded;
    const std::size_t slots = fresh_offset ? kSlots : 1;
    const std::size_t tail_padding = variant == PersistentVariant::FreshOffsetPadded ? 16 : 0;
    const std::size_t buffer_bytes = slots * kQuadBytes + tail_padding;

    GLuint vbo = 0;
    g.gen_buffers(1, &vbo);
    g.bind_vertex_array(vao);
    g.bind_buffer(kArrayBuffer, vbo);
    GLbitfield storage_flags = kMapWriteBit | kMapPersistentBit;
    GLbitfield map_flags = kMapWriteBit | kMapPersistentBit;
    storage_flags |= kMapCoherentBit;
    map_flags |= kMapCoherentBit;
    if (variant == PersistentVariant::ExplicitFlush) {
        map_flags |= kMapFlushExplicitBit;
    }
    g.buffer_storage(kArrayBuffer, static_cast<GLsizeiptr>(buffer_bytes), nullptr, storage_flags);
    auto* mapped = static_cast<unsigned char*>(
        g.map_buffer_range(kArrayBuffer, 0, static_cast<GLsizeiptr>(buffer_bytes), map_flags));
    if (mapped == nullptr) {
        std::printf("FRAME-TRANSITION-CANARY: rung=persistent-map variant=%s "
                    "FAIL (persistent map returned NULL)\n",
                    variant_name(variant));
        return false;
    }

    g.use_program(program);
    bool pass = true;
    for (int frame = 0; frame < kFrames; ++frame) {
        const std::size_t slot = fresh_offset ? static_cast<std::size_t>(frame) : 0;
        const std::size_t byte_offset = slot * kQuadBytes;
        auto* vertices = reinterpret_cast<QuadVertex*>(mapped + byte_offset);

        // Frame zero initializes the complete quad. Later in-place frames rewrite ONLY triangle
        // two, matching the suspected Blender composite pattern. A fresh ring slot necessarily
        // receives both triangles before its first use.
        if (frame == 0 || fresh_offset) {
            const QuadVertex lower[3] = {
                {-1.0f, -1.0f, 1.0f, 0.0f, 0.0f},
                {1.0f, -1.0f, 1.0f, 0.0f, 0.0f},
                {1.0f, 1.0f, 1.0f, 0.0f, 0.0f},
            };
            std::memcpy(vertices, lower, sizeof(lower));
        }
        const bool blue = (frame % 2) == 0;
        const QuadVertex upper[3] = {
            {-1.0f, -1.0f, 0.0f, blue ? 0.0f : 1.0f, blue ? 1.0f : 0.0f},
            {1.0f, 1.0f, 0.0f, blue ? 0.0f : 1.0f, blue ? 1.0f : 0.0f},
            {-1.0f, 1.0f, 0.0f, blue ? 0.0f : 1.0f, blue ? 1.0f : 0.0f},
        };
        std::memcpy(vertices + 3, upper, sizeof(upper));
        if (variant == PersistentVariant::ExplicitFlush) {
            const std::size_t flush_offset =
                byte_offset + (frame == 0 ? 0 : 3 * sizeof(QuadVertex));
            const std::size_t flush_size = frame == 0 ? kQuadBytes : sizeof(upper);
            g.flush_mapped_buffer_range(kArrayBuffer, static_cast<GLintptr>(flush_offset),
                                        static_cast<GLsizeiptr>(flush_size));
        }

        g.vertex_attrib_pointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(QuadVertex),
                                reinterpret_cast<const void*>(byte_offset));
        g.enable_vertex_attrib_array(0);
        g.vertex_attrib_pointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(QuadVertex),
                                reinterpret_cast<const void*>(byte_offset + 2 * sizeof(float)));
        g.enable_vertex_attrib_array(1);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glFinish();

        char rung[96] = {0};
        std::snprintf(rung, sizeof(rung), "persistent-map/%s", variant_name(variant));
        bool frame_ok = probe(rung, frame, "lower", 48, 16, 255, 0, 0);
        frame_ok = (blue ? probe(rung, frame, "upper", 16, 48, 0, 0, 255)
                         : probe(rung, frame, "upper", 16, 48, 0, 255, 0)) &&
                   frame_ok;
        pass = frame_ok && pass;
    }
    g.use_program(0);
    std::printf("FRAME-TRANSITION-CANARY: rung=persistent-map variant=%s %s\n",
                variant_name(variant), pass ? "PASS" : "FAIL");
    return pass;
}

bool run_persistent_map(const Gl& g) {
    const char* vs_source = "#version 120\n"
                            "attribute vec2 inPos; attribute vec3 inColor; varying vec3 color;\n"
                            "void main() { gl_Position=vec4(inPos,0.0,1.0); color=inColor; }\n";
    const char* fs_source = "#version 120\n"
                            "varying vec3 color; void main() { gl_FragColor=vec4(color,1.0); }\n";
    const GLuint vs = make_shader(g, kVertexShader, vs_source, "persistent vertex");
    const GLuint fs = make_shader(g, kFragmentShader, fs_source, "persistent fragment");
    if (vs == 0 || fs == 0) {
        return false;
    }
    const GLuint program = g.create_program();
    g.attach_shader(program, vs);
    g.attach_shader(program, fs);
    g.bind_attrib_location(program, 0, "inPos");
    g.bind_attrib_location(program, 1, "inColor");
    g.link_program(program);
    GLint linked = 0;
    g.get_program_iv(program, kLinkStatus, &linked);
    if (linked == 0) {
        char log[1024] = {0};
        g.get_program_log(program, sizeof(log) - 1, nullptr, log);
        std::printf("FRAME-TRANSITION-CANARY: FAIL (persistent program link: %s)\n", log);
        return false;
    }
    GLuint vao = 0;
    g.gen_vertex_arrays(1, &vao);
    bool pass = run_persistent_variant(g, program, vao, PersistentVariant::CoherentInPlace);
    pass = run_persistent_variant(g, program, vao, PersistentVariant::ExplicitFlush) && pass;
    pass = run_persistent_variant(g, program, vao, PersistentVariant::FreshOffset) && pass;
    pass = run_persistent_variant(g, program, vao, PersistentVariant::FreshOffsetPadded) && pass;
    return pass;
}

bool rung_requested(const std::string& requested, const char* rung) {
    return requested == "all" || requested == rung;
}

} // namespace

int main(int argc, char** argv) {
    std::string requested = "all";
    if (argc == 3 && std::strcmp(argv[1], "--rung") == 0) {
        requested = argv[2];
    } else if (argc != 1) {
        std::printf("FRAME-TRANSITION-CANARY: FAIL (usage: %s [--rung "
                    "all|clear-reuse|scissor-clear|attachment-rotate|vbo-update|persistent-map])\n",
                    argv[0]);
        return 2;
    }
    if (requested != "all" && requested != "clear-reuse" && requested != "scissor-clear" &&
        requested != "attachment-rotate" && requested != "vbo-update" &&
        requested != "persistent-map") {
        std::printf("FRAME-TRANSITION-CANARY: FAIL (unknown rung '%s')\n", requested.c_str());
        return 2;
    }

    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::printf("FRAME-TRANSITION-CANARY: FAIL (no display)\n");
        return 2;
    }
    const int screen = DefaultScreen(display);
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
    int count = 0;
    GLXFBConfig* configs = glXChooseFBConfig(display, screen, attrs, &count);
    if (configs == nullptr || count == 0) {
        std::printf("FRAME-TRANSITION-CANARY: FAIL (no GLX fbconfig)\n");
        return 2;
    }
    XVisualInfo* visual = glXGetVisualFromFBConfig(display, configs[0]);
    Colormap cmap =
        XCreateColormap(display, RootWindow(display, screen), visual->visual, AllocNone);
    XSetWindowAttributes window_attrs{};
    window_attrs.colormap = cmap;
    Window window =
        XCreateWindow(display, RootWindow(display, screen), 0, 0, kDim, kDim, 0, visual->depth,
                      InputOutput, visual->visual, CWColormap, &window_attrs);
    XMapWindow(display, window);
    XSync(display, False);

    auto create_context = reinterpret_cast<CreateContextAttribs>(gp("glXCreateContextAttribsARB"));
    GLXContext context = nullptr;
    if (create_context != nullptr) {
        int context_attrs[] = {0x2091, 3, 0x2092, 0, 0x9126, 0x2, None};
        context = create_context(display, configs[0], nullptr, True, context_attrs);
    }
    if (context == nullptr) {
        context = glXCreateContext(display, visual, nullptr, True);
    }
    if (context == nullptr || !glXMakeCurrent(display, window, context)) {
        std::printf("FRAME-TRANSITION-CANARY: FAIL (no current GL context)\n");
        return 2;
    }

    std::printf("FRAME-TRANSITION-CANARY: GL_VERSION=%s\n",
                reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    std::printf("FRAME-TRANSITION-CANARY: GL_RENDERER=%s\n",
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    Gl g;
    if (!load_gl(g)) {
        return 2;
    }

    GLuint fbo = 0;
    GLuint colors[2] = {0, 0};
    GLuint depths[2] = {0, 0};
    g.gen_framebuffers(1, &fbo);
    g.bind_framebuffer(kFramebuffer, fbo);
    g.gen_renderbuffers(2, colors);
    g.gen_renderbuffers(2, depths);
    for (int i = 0; i < 2; ++i) {
        g.bind_renderbuffer(kRenderbuffer, colors[i]);
        g.renderbuffer_storage(kRenderbuffer, GL_RGBA8, kDim, kDim);
        g.bind_renderbuffer(kRenderbuffer, depths[i]);
        g.renderbuffer_storage(kRenderbuffer, kDepthComponent24, kDim, kDim);
    }
    g.framebuffer_renderbuffer(kFramebuffer, kColorAttachment0, kRenderbuffer, colors[0]);
    g.framebuffer_renderbuffer(kFramebuffer, kDepthAttachment, kRenderbuffer, depths[0]);
    if (g.check_framebuffer_status(kFramebuffer) != kFramebufferComplete) {
        std::printf("FRAME-TRANSITION-CANARY: FAIL (FBO incomplete)\n");
        return 2;
    }
    glDrawBuffer(kColorAttachment0);
    glReadBuffer(kColorAttachment0);
    glViewport(0, 0, kDim, kDim);

    bool pass = true;
    if (rung_requested(requested, "clear-reuse")) {
        const bool ok = run_clear_reuse();
        std::printf("FRAME-TRANSITION-CANARY: rung=clear-reuse %s\n", ok ? "PASS" : "FAIL");
        pass = ok && pass;
    }
    if (rung_requested(requested, "scissor-clear")) {
        const bool ok = run_scissor_clear();
        std::printf("FRAME-TRANSITION-CANARY: rung=scissor-clear %s\n", ok ? "PASS" : "FAIL");
        pass = ok && pass;
    }
    if (rung_requested(requested, "attachment-rotate")) {
        const bool ok = run_attachment_rotation(g, colors, depths);
        std::printf("FRAME-TRANSITION-CANARY: rung=attachment-rotate %s\n", ok ? "PASS" : "FAIL");
        pass = ok && pass;
    }
    if (rung_requested(requested, "vbo-update")) {
        const bool ok = run_vbo_update(g);
        std::printf("FRAME-TRANSITION-CANARY: rung=vbo-update %s\n", ok ? "PASS" : "FAIL");
        pass = ok && pass;
    }
    if (rung_requested(requested, "persistent-map")) {
        const bool ok = run_persistent_map(g);
        std::printf("FRAME-TRANSITION-CANARY: rung=persistent-map %s\n", ok ? "PASS" : "FAIL");
        pass = ok && pass;
    }

    std::printf("FRAME-TRANSITION-CANARY: %s\n", pass ? "PASS" : "FAIL");
    glXMakeCurrent(display, None, nullptr);
    glXDestroyContext(display, context);
    XCloseDisplay(display);
    return pass ? 0 : 1;
}
