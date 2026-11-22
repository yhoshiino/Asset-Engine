// Physics adapter. The structs below are STAND-INS for the engine's own data structures
// (swap them for yours: only the Codec bodies must follow the field list).
// The file format is defined by the Codec, not by these structs: derived data
// (invMass, invInertia, bodyIndex) is never stored and is rebuilt after loading.
//
// Conventions: SI units (m, kg, s), right-handed, Y up; quaternion (x,y,z,w) unit length;
// inertia = diagonal of the body-space inertia tensor; velocities in world space;
// static bodies store mass = 0 and inertia = 0; references between objects use body ids (u64).
#pragma once
#include <cmath>
#include <unordered_map>
#include <vector>
#include "asset/archive.hpp"

namespace asset::physics {

struct Vec3 { float x = 0, y = 0, z = 0; };
struct Quat { float x = 0, y = 0, z = 0, w = 1; };
enum class ColliderKind : uint8_t { None = 0, Sphere = 1, Box = 2 };
struct Collider { ColliderKind kind = ColliderKind::None; float radius = 0; Vec3 halfExtents; };
struct Body {
    uint64_t id = 0;
    bool isStatic = false;
    Vec3 position; Quat orientation;
    float mass = 0; Vec3 inertia;
    Vec3 linearVelocity, angularVelocity;
    Collider collider;
    // derived, never serialised
    float invMass = 0; Vec3 invInertia;
};
struct Constraint { uint64_t bodyA = 0, bodyB = 0; float restLength = 0; };
struct World {
    Vec3 gravity{0, -9.81f, 0};
    float fixedTimestep = 1.0f / 60.0f;
    std::vector<Body> bodies;
    std::vector<Constraint> constraints;
    std::unordered_map<uint64_t, size_t> bodyIndex;   // derived

    void rebuildDerived() {
        bodyIndex.clear();
        for (size_t i = 0; i < bodies.size(); ++i) {
            Body& b = bodies[i];
            bodyIndex[b.id] = i;
            b.invMass = b.isStatic ? 0.f : 1.f / b.mass;
            b.invInertia = b.isStatic ? Vec3{} : Vec3{1.f / b.inertia.x, 1.f / b.inertia.y, 1.f / b.inertia.z};
        }
    }
};
struct Snapshot { uint64_t stepIndex = 0; double simTime = 0; World world; };   // taken at the END of a step

} // namespace asset::physics

namespace asset {
namespace detail {
using namespace physics;
inline void wVec(ByteWriter& w, Vec3 v) { w.f32(v.x); w.f32(v.y); w.f32(v.z); }
inline Vec3 rVec(ByteReader& r) { Vec3 v; v.x = r.f32Finite(); v.y = r.f32Finite(); v.z = r.f32Finite(); return v; }

inline void writeWorld(ByteWriter& w, const World& world) {
    wVec(w, world.gravity);
    w.f32(world.fixedTimestep);
    w.count(world.bodies.size());
    for (const Body& b : world.bodies) {
        w.u64(b.id); w.boolean(b.isStatic);
        wVec(w, b.position);
        w.f32(b.orientation.x); w.f32(b.orientation.y); w.f32(b.orientation.z); w.f32(b.orientation.w);
        w.f32(b.mass); wVec(w, b.inertia);
        wVec(w, b.linearVelocity); wVec(w, b.angularVelocity);
        w.u8(static_cast<uint8_t>(b.collider.kind)); w.f32(b.collider.radius); wVec(w, b.collider.halfExtents);
    }
    w.count(world.constraints.size());
    for (const Constraint& c : world.constraints) { w.u64(c.bodyA); w.u64(c.bodyB); w.f32(c.restLength); }
}

constexpr size_t kBodyBytes = 8 + 1 + 12 + 16 + 4 + 12 + 12 + 12 + 1 + 4 + 12;   // = 94
constexpr size_t kConstraintBytes = 8 + 8 + 4;                                    // = 20

inline World readWorld(ByteReader& r) {
    World world;
    world.gravity = rVec(r);
    world.fixedTimestep = r.f32Finite();
    if (!(world.fixedTimestep > 0)) throw ArchiveError(Err::BadValue, "fixed timestep must be > 0");
    uint32_t nb = r.count(kBodyBytes);
    world.bodies.reserve(nb);
    std::unordered_map<uint64_t, bool> seen;
    for (uint32_t i = 0; i < nb; ++i) {
        Body b;
        b.id = r.u64();
        if (!seen.emplace(b.id, true).second) throw ArchiveError(Err::Duplicate, "duplicate body id " + std::to_string(b.id));
        b.isStatic = r.boolean();
        b.position = rVec(r);
        b.orientation = {r.f32Finite(), r.f32Finite(), r.f32Finite(), r.f32Finite()};
        float n2 = b.orientation.x * b.orientation.x + b.orientation.y * b.orientation.y
                 + b.orientation.z * b.orientation.z + b.orientation.w * b.orientation.w;
        if (std::fabs(n2 - 1.0f) > 1e-3f) throw ArchiveError(Err::BadValue, "orientation is not a unit quaternion");
        float inv = 1.0f / std::sqrt(n2);
        b.orientation = {b.orientation.x * inv, b.orientation.y * inv, b.orientation.z * inv, b.orientation.w * inv};
        b.mass = r.f32Finite(); b.inertia = rVec(r);
        if (b.isStatic) {
            if (b.mass != 0 || b.inertia.x != 0 || b.inertia.y != 0 || b.inertia.z != 0)
                throw ArchiveError(Err::BadValue, "static body must have zero mass and inertia");
        } else if (!(b.mass > 0 && b.inertia.x > 0 && b.inertia.y > 0 && b.inertia.z > 0)) {
            throw ArchiveError(Err::BadValue, "dynamic body needs positive mass and inertia");
        }
        b.linearVelocity = rVec(r); b.angularVelocity = rVec(r);
        uint8_t kind = r.u8();
        if (kind > static_cast<uint8_t>(ColliderKind::Box)) throw ArchiveError(Err::BadValue, "unknown collider kind");
        b.collider.kind = static_cast<ColliderKind>(kind);
        b.collider.radius = r.f32Finite(); b.collider.halfExtents = rVec(r);
        if (b.collider.kind == ColliderKind::Sphere && !(b.collider.radius > 0)) throw ArchiveError(Err::BadValue, "sphere radius must be > 0");
        if (b.collider.kind == ColliderKind::Box && !(b.collider.halfExtents.x > 0 && b.collider.halfExtents.y > 0 && b.collider.halfExtents.z > 0))
            throw ArchiveError(Err::BadValue, "box half extents must be > 0");
        world.bodies.push_back(b);
    }
    uint32_t nc = r.count(kConstraintBytes);
    world.constraints.reserve(nc);
    for (uint32_t i = 0; i < nc; ++i) {
        Constraint c;
        c.bodyA = r.u64(); c.bodyB = r.u64(); c.restLength = r.f32Finite();
        if (!seen.count(c.bodyA) || !seen.count(c.bodyB)) throw ArchiveError(Err::BadReference, "constraint references an unknown body id");
        if (c.bodyA == c.bodyB) throw ArchiveError(Err::BadReference, "constraint links a body to itself");
        world.constraints.push_back(c);
    }
    world.rebuildDerived();       // rebuild derived data (id -> index, inverse mass/inertia)
    return world;
}
} // namespace detail

template <> struct Codec<physics::World> {
    static constexpr uint32_t kType = 100;
    static constexpr uint16_t kVersion = 1;
    static void write(ByteWriter& w, const physics::World& v) { detail::writeWorld(w, v); }
    static physics::World read(ByteReader& r, uint16_t) { return detail::readWorld(r); }
};
template <> struct Codec<physics::Snapshot> {
    static constexpr uint32_t kType = 101;
    static constexpr uint16_t kVersion = 1;
    static void write(ByteWriter& w, const physics::Snapshot& s) { w.u64(s.stepIndex); w.f64(s.simTime); detail::writeWorld(w, s.world); }
    static physics::Snapshot read(ByteReader& r, uint16_t) {
        physics::Snapshot s;
        s.stepIndex = r.u64(); s.simTime = r.f64();
        if (!std::isfinite(s.simTime)) throw ArchiveError(Err::BadValue, "non-finite simulation time");
        s.world = detail::readWorld(r);
        return s;
    }
};
} // namespace asset
