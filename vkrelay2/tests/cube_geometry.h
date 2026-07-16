// Shared cube geometry + MVP math for the literal-vkcube-shaped artifacts: the on-screen
// `vkrelay2-cube` canary and the in-process `integration_real_cube`
// regression. Both drive the SAME bufferless cube whose geometry lives inside the uniform buffer
// (the cube_spv.h shader shape), so the 1216-byte UBO layout MUST agree byte-for-byte -- keeping it
// in one header removes the drift risk of inlining it twice.
//
// Layout (std140, matching cube_spv.h's decorations): mvp mat4 @ 0 (64 bytes), position[36] vec4 @
// 64, attr[36] vec4 @ 640 -> 1216 bytes total. The shader reads texcoord = attr[i].xy and
// gl_Position = mvp * position[i], so position carries xyz (w = 1) and attr carries uv (zw = 0).
//
// No Vulkan dependency on purpose: this is pure host math/data, shared by a loader-side canary and
// a worker-side test.
#ifndef VKRELAY2_TESTS_CUBE_GEOMETRY_H
#define VKRELAY2_TESTS_CUBE_GEOMETRY_H

#include <cmath>
#include <cstddef>
#include <cstring>

namespace vkr::cube_geom {

inline constexpr int kCubeVertices = 36; // 6 faces * 2 triangles * 3 vertices
// 16 (mvp) + 36*4 (position) + 36*4 (attr) floats.
inline constexpr std::size_t kUboFloats = 16 + kCubeVertices * 4 + kCubeVertices * 4;
inline constexpr std::size_t kUboBytes = kUboFloats * sizeof(float); // 1216

// The 8 corners of a unit cube, then 6 faces (each a quad A,B,C,A,C,D wound CCW from outside). A
// checkerboard texture makes the per-face orientation irrelevant; every face spans the full
// [0,1]^2, so depth (not winding) is what sorts the cube -- the canary/test draw with cull NONE.
inline constexpr float kFacePos[kCubeVertices][3] = {
    // +X
    {1, -1, -1},
    {1, 1, -1},
    {1, 1, 1},
    {1, -1, -1},
    {1, 1, 1},
    {1, -1, 1},
    // -X
    {-1, -1, -1},
    {-1, -1, 1},
    {-1, 1, 1},
    {-1, -1, -1},
    {-1, 1, 1},
    {-1, 1, -1},
    // +Y
    {-1, 1, -1},
    {-1, 1, 1},
    {1, 1, 1},
    {-1, 1, -1},
    {1, 1, 1},
    {1, 1, -1},
    // -Y
    {-1, -1, -1},
    {1, -1, -1},
    {1, -1, 1},
    {-1, -1, -1},
    {1, -1, 1},
    {-1, -1, 1},
    // +Z
    {-1, -1, 1},
    {1, -1, 1},
    {1, 1, 1},
    {-1, -1, 1},
    {1, 1, 1},
    {-1, 1, 1},
    // -Z
    {-1, -1, -1},
    {-1, 1, -1},
    {1, 1, -1},
    {-1, -1, -1},
    {1, 1, -1},
    {1, -1, -1},
};
// One UV per vertex; the same 6-entry quad pattern repeats on every face (each face is A,B,C,A,C,D
// with A=(0,0) B=(1,0) C=(1,1) D=(0,1)).
inline constexpr float kFaceUv[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};

// --- Minimal column-major 4x4 math (flat index = col*4 + row), Vulkan clip space ---

// out = a * b (column-major).
inline void mat4_mul(const float a[16], const float b[16], float out[16]) {
    float r[16];
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) {
                s += a[k * 4 + row] * b[c * 4 + k];
            }
            r[c * 4 + row] = s;
        }
    }
    std::memcpy(out, r, sizeof(r));
}

// Right-handed perspective into Vulkan NDC (z in [0,1], y flipped so +y is down in clip space).
inline void perspective(float fovy_rad, float aspect, float znear, float zfar, float out[16]) {
    const float f = 1.0f / std::tan(fovy_rad * 0.5f);
    std::memset(out, 0, sizeof(float) * 16);
    out[0] = f / aspect;                       // col0,row0
    out[5] = -f;                               // col1,row1 (Vulkan y-flip)
    out[10] = zfar / (znear - zfar);           // col2,row2
    out[11] = -1.0f;                           // col2,row3
    out[14] = (znear * zfar) / (znear - zfar); // col3,row2
}

inline void translate(float x, float y, float z, float out[16]) {
    std::memset(out, 0, sizeof(float) * 16);
    out[0] = out[5] = out[10] = out[15] = 1.0f;
    out[12] = x;
    out[13] = y;
    out[14] = z;
}

inline void rotate_y(float a, float out[16]) {
    const float c = std::cos(a), s = std::sin(a);
    std::memset(out, 0, sizeof(float) * 16);
    out[0] = c;
    out[2] = -s;
    out[8] = s;
    out[10] = c;
    out[5] = out[15] = 1.0f;
}

inline void rotate_x(float a, float out[16]) {
    const float c = std::cos(a), s = std::sin(a);
    std::memset(out, 0, sizeof(float) * 16);
    out[5] = c;
    out[6] = s;
    out[9] = -s;
    out[10] = c;
    out[0] = out[15] = 1.0f;
}

// The spinning MVP for `frame`: a fixed-tilt cube pushed back from the camera, rotating about Y.
// The depth buffer is exercised because back faces sit behind front faces along the view ray.
inline void mvp(float angle, float out[16]) {
    float proj[16], view[16], roty[16], rotx[16], model[16], vm[16];
    perspective(1.0472f /* 60 deg */, 1.0f, 0.1f, 100.0f, proj);
    translate(0.0f, 0.0f, -5.0f, view);
    rotate_y(angle, roty);
    rotate_x(0.5f, rotx);
    mat4_mul(rotx, roty, model); // model = tilt * spin
    mat4_mul(view, model, vm);   // view * model
    mat4_mul(proj, vm, out);     // proj * view * model
}

// Fill `out` (kUboFloats floats) with the frame's MVP + the cube geometry (position xyz/w=1, attr
// uv/zw=0), matching cube_spv.h's std140 offsets.
inline void build_ubo(float angle, float* out) {
    mvp(angle, out); // mvp @ float 0..15
    float* pos = out + 16;
    float* attr = out + 16 + kCubeVertices * 4;
    for (int i = 0; i < kCubeVertices; ++i) {
        pos[i * 4 + 0] = kFacePos[i][0];
        pos[i * 4 + 1] = kFacePos[i][1];
        pos[i * 4 + 2] = kFacePos[i][2];
        pos[i * 4 + 3] = 1.0f;
        attr[i * 4 + 0] = kFaceUv[i % 6][0];
        attr[i * 4 + 1] = kFaceUv[i % 6][1];
        attr[i * 4 + 2] = 0.0f;
        attr[i * 4 + 3] = 0.0f;
    }
}

} // namespace vkr::cube_geom

#endif // VKRELAY2_TESTS_CUBE_GEOMETRY_H
