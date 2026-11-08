#pragma once
// Engine-style quad UV mapping shared by the D3D11 world pixel shader and the C++ unit tests.
//
// FUN_0042e850 does not interpolate a quad as two triangles. It walks two vertex chains from the
// top vertex (FUN_0042e4d0 fills one edge table per chain), takes the left and right edge values
// of each scanline, and interpolates u/v linearly across the span. QuadMapUv reproduces that
// mapping for a continuous position (curX, curY) in 640x480 pixel units:
//   - edges whose next vertex is lower (index order) form one chain, edges whose next vertex is
//     higher form the other; horizontal edges are skipped like the engine's flat-top walk;
//   - when the chains cross (reversed winding) the left/right edges are swapped per scanline,
//     like the span loop of FUN_0042e850 swaps its two edge-table entries;
//   - u/v are linear in x between the two edges of the scanline.
//
// The function body is written once in the common subset of C++17 and HLSL SM4 inside the
// RR_QUAD_MAPPING_SOURCE macro. C++ instantiates it below (namespace rr::qm, with a tiny float2
// shim), and the presenter prepends RR_QUAD_MAPPING_HLSL to its world pixel shader, so the shader
// and the tests execute the same mapping source. Only block comments may appear inside the macro.

#include <cstdint>

namespace rr {
namespace qm {

struct float2 {
    float x;
    float y;
    float2() : x(0.0f), y(0.0f) {}
    float2(float ax, float ay) : x(ax), y(ay) {}
};

inline float2 operator+(float2 a, float2 b) { return float2(a.x + b.x, a.y + b.y); }
inline float2 operator-(float2 a, float2 b) { return float2(a.x - b.x, a.y - b.y); }
inline float2 operator*(float2 a, float s) { return float2(a.x * s, a.y * s); }
inline float2 operator*(float s, float2 a) { return float2(a.x * s, a.y * s); }
inline float2 operator/(float2 a, float s) { return float2(a.x / s, a.y / s); }
inline float min(float a, float b) { return (a < b) ? a : b; }
inline float max(float a, float b) { return (a > b) ? a : b; }
inline float clamp(float v, float lo, float hi) { return min(max(v, lo), hi); }
inline float saturate(float v) { return clamp(v, 0.0f, 1.0f); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float2 lerp(float2 a, float2 b, float t) { return float2(lerp(a.x, b.x, t), lerp(a.y, b.y, t)); }

}  // namespace qm
}  // namespace rr

#define RR_QM_EMPTY
#define RR_QM_STRINGIFY_IMPL(...) #__VA_ARGS__
#define RR_QM_STRINGIFY(...) RR_QM_STRINGIFY_IMPL(__VA_ARGS__)

/* QuadUvSample fields:
     uv     mapped u/v at (curX, curY), with curY clamped to the quad's y range and the span
            parameter saturated (the value the shader samples);
     dUvDx  analytic d(uv)/dx of the scanline mapping, in uv units per 640x480 pixel;
     dUvDy  analytic d(uv)/dy of the scanline mapping at curX, in uv units per 640x480 pixel;
     xL, xR left and right edge x of the scanline used.
   The derivatives come from the selected edges, never from the clamped or saturated value, so a
   pixel just outside the quad does not produce a derivative spike. */
#define RR_QUAD_MAPPING_SOURCE(FN)                                                            \
struct QuadUvSample {                                                                         \
    float2 uv;                                                                                \
    float2 dUvDx;                                                                             \
    float2 dUvDy;                                                                             \
    float xL;                                                                                 \
    float xR;                                                                                 \
};                                                                                            \
FN QuadUvSample QuadMapUv(float2 pts[4], float2 uvs[4], float curX, float curY) {             \
    float ymin = min(min(pts[0].y, pts[1].y), min(pts[2].y, pts[3].y));                       \
    float ymax = max(max(pts[0].y, pts[1].y), max(pts[2].y, pts[3].y));                       \
    float yc = clamp(curY, ymin, ymax);                                                       \
    float xL = 0.0f;                                                                          \
    float xR = 0.0f;                                                                          \
    float dxL = 0.0f;                                                                         \
    float dxR = 0.0f;                                                                         \
    float2 uvL = float2(0.0f, 0.0f);                                                          \
    float2 uvR = float2(0.0f, 0.0f);                                                          \
    float2 duvL = float2(0.0f, 0.0f);                                                         \
    float2 duvR = float2(0.0f, 0.0f);                                                         \
    bool foundL = false;                                                                      \
    bool foundR = false;                                                                      \
    for (int i = 0; i < 4; ++i) {                                                             \
        int nx = (i + 1) & 3;                                                                 \
        float2 a = pts[i];                                                                    \
        float2 b = pts[nx];                                                                   \
        float2 ua = uvs[i];                                                                   \
        float2 ub = uvs[nx];                                                                  \
        if (b.y > a.y) {                                                                      \
            if (yc >= a.y && yc <= b.y && !foundR) {                                          \
                float s = (yc - a.y) / (b.y - a.y);                                           \
                xR = lerp(a.x, b.x, s);                                                       \
                uvR = lerp(ua, ub, s);                                                        \
                dxR = (b.x - a.x) / (b.y - a.y);                                              \
                duvR = (ub - ua) / (b.y - a.y);                                               \
                foundR = true;                                                                \
            }                                                                                 \
        } else if (b.y < a.y) {                                                               \
            if (yc >= b.y && yc <= a.y && !foundL) {                                          \
                float s = (yc - b.y) / (a.y - b.y);                                           \
                xL = lerp(b.x, a.x, s);                                                       \
                uvL = lerp(ub, ua, s);                                                        \
                dxL = (a.x - b.x) / (a.y - b.y);                                              \
                duvL = (ua - ub) / (a.y - b.y);                                               \
                foundL = true;                                                                \
            }                                                                                 \
        }                                                                                     \
    }                                                                                         \
    if (!foundL) {                                                                            \
        xL = min(min(pts[0].x, pts[1].x), min(pts[2].x, pts[3].x));                           \
        uvL = uvs[0];                                                                         \
    }                                                                                         \
    if (!foundR) {                                                                            \
        xR = max(max(pts[0].x, pts[1].x), max(pts[2].x, pts[3].x));                           \
        uvR = uvs[1];                                                                         \
    }                                                                                         \
    if (xR < xL) {                                                                            \
        float tx = xL;                                                                        \
        xL = xR;                                                                              \
        xR = tx;                                                                              \
        float tdx = dxL;                                                                      \
        dxL = dxR;                                                                            \
        dxR = tdx;                                                                            \
        float2 tuv = uvL;                                                                     \
        uvL = uvR;                                                                            \
        uvR = tuv;                                                                            \
        float2 tduv = duvL;                                                                   \
        duvL = duvR;                                                                          \
        duvR = tduv;                                                                          \
    }                                                                                         \
    float span = max(xR - xL, 1e-4f);                                                         \
    float t = saturate((curX - xL) / span);                                                   \
    float2 slope = (uvR - uvL) / span;                                                        \
    float2 dSlope = ((duvR - duvL) - slope * (dxR - dxL)) / span;                             \
    QuadUvSample o;                                                                           \
    o.uv = lerp(uvL, uvR, t);                                                                 \
    o.dUvDx = slope;                                                                          \
    o.dUvDy = (duvL - slope * dxL) + dSlope * (curX - xL);                                    \
    o.xL = xL;                                                                                \
    o.xR = xR;                                                                                \
    return o;                                                                                 \
}

namespace rr {
namespace qm {
RR_QUAD_MAPPING_SOURCE(inline)
}  // namespace qm
}  // namespace rr

// HLSL text of the same mapping, prepended to the world pixel shader source.
#define RR_QUAD_MAPPING_HLSL RR_QM_STRINGIFY(RR_QUAD_MAPPING_SOURCE(RR_QM_EMPTY))

// Texture addressing for one textured world quad (T5d).
// - Atlas quads (a non-dynamic PDAT whose uv_box is a non-empty strict sub-rectangle of the
//   texture) clamp to the uv_box, so neighbouring atlas cells never bleed in.
// - Every other quad wraps when any vertex u/v lies outside [0, size] (the engine span routines
//   mask texel coordinates with the texture size, so they wrap too), and otherwise clamps to the
//   texture rectangle. Dynamic sprites are cropped to their uv_box at capture time, so their
//   texture rectangle already is the box.
// clampTexel holds inclusive texel indices {minX, minY, maxX, maxY}.
struct QuadAddressing {
    bool wrap = false;
    bool atlas = false;
    int32_t clampTexel[4] = {0, 0, 0, 0};
};

inline QuadAddressing ChooseQuadAddressing(const int32_t uvBox[4],
                                           bool dynamicSprite,
                                           int32_t texW,
                                           int32_t texH,
                                           const int32_t us[4],
                                           const int32_t vs[4]) {
    QuadAddressing a;
    const int32_t w = (texW > 0) ? texW : 1;
    const int32_t h = (texH > 0) ? texH : 1;
    a.clampTexel[0] = 0;
    a.clampTexel[1] = 0;
    a.clampTexel[2] = w - 1;
    a.clampTexel[3] = h - 1;

    const bool boxValid = !dynamicSprite && uvBox && uvBox[2] > uvBox[0] && uvBox[3] > uvBox[1];
    if (boxValid) {
        const bool strictSubRect = uvBox[0] > 0 || uvBox[1] > 0 || uvBox[2] < w - 1 || uvBox[3] < h - 1;
        if (strictSubRect) {
            a.atlas = true;
            a.clampTexel[0] = uvBox[0];
            a.clampTexel[1] = uvBox[1];
            a.clampTexel[2] = uvBox[2];
            a.clampTexel[3] = uvBox[3];
            return a;
        }
    }

    bool outside = false;
    for (int i = 0; i < 4; ++i) {
        if (us[i] < 0 || us[i] > w || vs[i] < 0 || vs[i] > h) {
            outside = true;
        }
    }
    a.wrap = outside;
    return a;
}
