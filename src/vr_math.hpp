#pragma once

#include <cmath>

// Minimal math for the VR camera. Conventions match the game (GX/dolphin):
//   Mtx34:  row-major 3x4 affine, column vectors: v' = M * [v, 1]
//   Mtx44f: row-major 4x4, clip = P * [v, 1]
// OpenXR poses (quaternion + position) are converted into the same convention.
namespace vr {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) {
    const float l = length(a);
    return l > 1e-8f ? a * (1.0f / l) : Vec3{0, 0, 0};
}

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

struct Pose {
    Quat orientation;
    Vec3 position;
};

struct Mtx34 {
    float m[3][4]{};
};

struct Mtx44f {
    float m[4][4]{};
};

inline Mtx34 identity34() {
    Mtx34 r;
    r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0f;
    return r;
}

// R such that v_rotated = R * v.
inline void quat_to_rot(const Quat& q, float r[3][3]) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    r[0][0] = 1 - 2 * (y * y + z * z);
    r[0][1] = 2 * (x * y - z * w);
    r[0][2] = 2 * (x * z + y * w);
    r[1][0] = 2 * (x * y + z * w);
    r[1][1] = 1 - 2 * (x * x + z * z);
    r[1][2] = 2 * (y * z - x * w);
    r[2][0] = 2 * (x * z - y * w);
    r[2][1] = 2 * (y * z + x * w);
    r[2][2] = 1 - 2 * (x * x + y * y);
}

inline Quat quat_from_yaw(float yaw) {
    // Rotation about +Y by `yaw` radians.
    return {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
}

// Yaw (radians about +Y) of the forward (-Z) axis of a rotation.
inline float yaw_of(const Quat& q) {
    float r[3][3];
    quat_to_rot(q, r);
    const Vec3 fwd{-r[0][2], -r[1][2], -r[2][2]};
    return std::atan2(-fwd.x, -fwd.z);
}

inline Vec3 rotate(const Quat& q, Vec3 v) {
    float r[3][3];
    quat_to_rot(q, r);
    return {r[0][0] * v.x + r[0][1] * v.y + r[0][2] * v.z, r[1][0] * v.x + r[1][1] * v.y + r[1][2] * v.z,
        r[2][0] * v.x + r[2][1] * v.y + r[2][2] * v.z};
}

inline Quat quat_mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// Pose -> matrix mapping pose-local coordinates to the parent space (local-to-parent).
inline Mtx34 pose_to_mtx(const Pose& p, float positionScale = 1.0f) {
    float r[3][3];
    quat_to_rot(p.orientation, r);
    Mtx34 m;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            m.m[i][j] = r[i][j];
        }
    }
    m.m[0][3] = p.position.x * positionScale;
    m.m[1][3] = p.position.y * positionScale;
    m.m[2][3] = p.position.z * positionScale;
    return m;
}

// Inverse of a rigid (rotation + translation) transform.
inline Mtx34 inverse_rigid(const Mtx34& a) {
    Mtx34 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.m[i][j] = a.m[j][i];
        }
    }
    for (int i = 0; i < 3; ++i) {
        r.m[i][3] = -(r.m[i][0] * a.m[0][3] + r.m[i][1] * a.m[1][3] + r.m[i][2] * a.m[2][3]);
    }
    return r;
}

// General affine inverse (for game view matrices that may carry scale).
inline Mtx34 inverse_affine(const Mtx34& a) {
    const float(*m)[4] = a.m;
    const float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                      m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                      m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    Mtx34 r;
    if (std::fabs(det) < 1e-12f) {
        return identity34();
    }
    const float inv = 1.0f / det;
    r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv;
    r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv;
    r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv;
    r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv;
    r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv;
    r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv;
    r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv;
    r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv;
    r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv;
    for (int i = 0; i < 3; ++i) {
        r.m[i][3] = -(r.m[i][0] * m[0][3] + r.m[i][1] * m[1][3] + r.m[i][2] * m[2][3]);
    }
    return r;
}

inline Mtx34 mul(const Mtx34& a, const Mtx34& b) {
    Mtx34 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + (j == 3 ? a.m[i][3] : 0.0f);
        }
    }
    return r;
}

inline Mtx44f mul(const Mtx44f& a, const Mtx34& b) {
    Mtx44f r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + (j == 3 ? a.m[i][3] : 0.0f);
        }
    }
    return r;
}

inline Mtx44f mul(const Mtx44f& a, const Mtx44f& b) {
    Mtx44f r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        }
    }
    return r;
}

inline Mtx44f to44(const Mtx34& a) {
    Mtx44f r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            r.m[i][j] = a.m[i][j];
        }
    }
    r.m[3][3] = 1.0f;
    return r;
}

// Tangents of an asymmetric frustum (OpenXR XrFovf angles converted with tan()).
struct Fov {
    float tanLeft = -1, tanRight = 1, tanUp = 1, tanDown = -1;
};

inline constexpr float kPi = 3.14159265358979f;

} // namespace vr
