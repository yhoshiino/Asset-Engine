// Adapter for the Moteur3DCPU scene (ECS: Transform, MeshRenderer, Velocity, Spin, Camera).
// Include path must contain <Moteur3DCPU>/header (vec.h, obj.h, texture.h, ecs/, components/).
//
// What is persisted (and what is NOT):
//  * Mesh (type 200) and Texture (type 201) are independent ASSETS, stored once even if several
//    entities share them. MeshRenderer's raw pointers are replaced by asset ids (0 = nullptr).
//  * The scene (type 202) = list of entities, each with a presence mask + its components.
//  * Runtime ids are NEVER persisted: entity ids are re-assigned on load (ECS ids are recycled),
//    component-type ids (order of addComponentType) are replaced by fixed bits of the mask.
//  * Conventions: rotation_y in radians around +Y, uniform scale > 0, camera yaw/pitch in radians
//    (pitch within +-pi/2), mesh indices 0-based, uv index stored +1 (0 = no uv), texture pixels are
//    the engine's packed ARGB8888 uint32 (a<<24|r<<16|g<<8|b), row 0 first, stored as u32 LE values.
#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "asset/archive.hpp"
#include "vec.h"
#include "obj.h"
#include "texture.h"
#include "components/components.h"
#include "ecs/entityManager.h"

namespace asset::scene3d {
struct RenderDesc { uint64_t meshAsset = 0, textureAsset = 0; uint32_t material = 0; };
struct EntityDesc {
    uint32_t id = 0;                       // id at save time (informational: remapped on load)
    std::optional<Transform> transform;
    std::optional<RenderDesc> render;
    std::optional<Velocity> velocity;
    std::optional<Spin> spin;
    std::optional<Camera> camera;
};
struct SceneDesc { std::vector<EntityDesc> entities; };
} // namespace asset::scene3d

namespace asset {
namespace detail3d {
inline void wV3(ByteWriter& w, const vec3& v) { w.f32(v.x); w.f32(v.y); w.f32(v.z); }
inline vec3 rV3(ByteReader& r) { vec3 v; v.x = r.f32Finite(); v.y = r.f32Finite(); v.z = r.f32Finite(); return v; }
constexpr float kHalfPi = 1.5707964f;
}

template <> struct Codec<Mesh> {
    static constexpr uint32_t kType = 200;
    static constexpr uint16_t kVersion = 1;
    // verts: n*3 f32 | uvs: n*2 f32 | corners: n * (vertexIndex u32, uvIndex+1 u32)
    static void write(ByteWriter& w, const Mesh& m) {
        if (m.faces.size() % 3 != 0 || m.face_uvs.size() != m.faces.size())
            throw ArchiveError(Err::BadValue, "mesh: inconsistent face arrays");
        w.count(m.verts.size());
        for (const vec3& v : m.verts) detail3d::wV3(w, v);
        w.count(m.uvs.size());
        for (const vec2& t : m.uvs) { w.f32(t.x); w.f32(t.y); }
        w.count(m.faces.size());
        for (size_t i = 0; i < m.faces.size(); ++i) {
            int vi = m.faces[i], ti = m.face_uvs[i];
            if (vi < 0 || static_cast<size_t>(vi) >= m.verts.size()) throw ArchiveError(Err::BadReference, "mesh: vertex index out of range");
            if (ti < -1 || (ti >= 0 && static_cast<size_t>(ti) >= m.uvs.size())) throw ArchiveError(Err::BadReference, "mesh: uv index out of range");
            w.u32(static_cast<uint32_t>(vi)); w.u32(static_cast<uint32_t>(ti + 1));
        }
    }
    static Mesh read(ByteReader& r, uint16_t) {
        Mesh m;
        uint32_t nv = r.count(12);
        m.verts.reserve(nv);
        for (uint32_t i = 0; i < nv; ++i) m.verts.push_back(detail3d::rV3(r));
        uint32_t nu = r.count(8);
        m.uvs.reserve(nu);
        for (uint32_t i = 0; i < nu; ++i) { vec2 t; t.x = r.f32Finite(); t.y = r.f32Finite(); m.uvs.push_back(t); }
        uint32_t ni = r.count(8);
        if (ni % 3 != 0) throw ArchiveError(Err::BadValue, "mesh: corner count not a multiple of 3");
        m.faces.reserve(ni); m.face_uvs.reserve(ni);
        for (uint32_t i = 0; i < ni; ++i) {
            uint32_t vi = r.u32(), tp = r.u32();
            if (vi >= nv) throw ArchiveError(Err::BadReference, "mesh: vertex index out of range");
            if (tp > nu) throw ArchiveError(Err::BadReference, "mesh: uv index out of range");
            m.faces.push_back(static_cast<int>(vi)); m.face_uvs.push_back(static_cast<int>(tp) - 1);
        }
        return m;
    }
};

template <> struct Codec<Texture> {
    static constexpr uint32_t kType = 201;
    static constexpr uint16_t kVersion = 1;
    static constexpr uint32_t kMaxDim = 16384;
    static void write(ByteWriter& w, const Texture& t) {
        if (t.width <= 0 || t.height <= 0 || static_cast<uint32_t>(t.width) > kMaxDim || static_cast<uint32_t>(t.height) > kMaxDim ||
            t.pixels.size() != static_cast<size_t>(t.width) * static_cast<size_t>(t.height))
            throw ArchiveError(Err::BadValue, "texture: invalid dimensions");
        w.reserve(8 + t.pixels.size() * 4);
        w.u32(static_cast<uint32_t>(t.width)); w.u32(static_cast<uint32_t>(t.height));
        for (uint32_t px : t.pixels) w.u32(px);
    }
    static Texture read(ByteReader& r, uint16_t) {
        uint32_t w = r.u32(), h = r.u32();
        if (w == 0 || h == 0 || w > kMaxDim || h > kMaxDim) throw ArchiveError(Err::BadValue, "texture: invalid dimensions");
        uint64_t n = static_cast<uint64_t>(w) * h;
        if (n > r.remaining() / 4) throw ArchiveError(Err::BadSize, "texture: pixel data larger than asset");   // before allocating
        Texture t; t.width = static_cast<int>(w); t.height = static_cast<int>(h);
        t.pixels.resize(static_cast<size_t>(n));
        for (uint32_t& px : t.pixels) px = r.u32();
        return t;
    }
};

template <> struct Codec<scene3d::SceneDesc> {
    static constexpr uint32_t kType = 202;
    static constexpr uint16_t kVersion = 1;
    // entity: id u32 | mask u8 (bit0 Transform, 1 MeshRenderer, 2 Velocity, 3 Spin, 4 Camera; bits 5-7 = 0)
    //   Transform: pos 3f | rotation_y f | scale f      MeshRenderer: meshAsset u64 | textureAsset u64 | material u32
    //   Velocity: 3f                                     Spin: f
    //   Camera: pos 3f | yaw | pitch | speed | sensitivity
    static void write(ByteWriter& w, const scene3d::SceneDesc& s) {
        w.count(s.entities.size());
        for (const auto& e : s.entities) {
            w.u32(e.id);
            w.u8(static_cast<uint8_t>((e.transform ? 1 : 0) | (e.render ? 2 : 0) | (e.velocity ? 4 : 0) | (e.spin ? 8 : 0) | (e.camera ? 16 : 0)));
            if (e.transform) { detail3d::wV3(w, e.transform->position); w.f32(e.transform->rotation_y); w.f32(e.transform->scale); }
            if (e.render) { w.u64(e.render->meshAsset); w.u64(e.render->textureAsset); w.u32(e.render->material); }
            if (e.velocity) detail3d::wV3(w, e.velocity->value);
            if (e.spin) w.f32(e.spin->speed_y);
            if (e.camera) { detail3d::wV3(w, e.camera->position); w.f32(e.camera->yaw); w.f32(e.camera->pitch); w.f32(e.camera->speed); w.f32(e.camera->sensitivity); }
        }
    }
    static scene3d::SceneDesc read(ByteReader& r, uint16_t) {
        scene3d::SceneDesc s;
        uint32_t n = r.count(5);
        s.entities.reserve(n);
        std::unordered_set<uint32_t> seen;
        for (uint32_t i = 0; i < n; ++i) {
            scene3d::EntityDesc e;
            e.id = r.u32();
            if (e.id == 0) throw ArchiveError(Err::BadValue, "entity id 0 is invalid");
            if (!seen.insert(e.id).second) throw ArchiveError(Err::Duplicate, "duplicate entity id " + std::to_string(e.id));
            uint8_t mask = r.u8();
            if (mask & 0xE0) throw ArchiveError(Err::BadValue, "unknown component bits");
            if (mask & 1) {
                Transform t; t.position = detail3d::rV3(r); t.rotation_y = r.f32Finite(); t.scale = r.f32Finite();
                if (!(t.scale > 0)) throw ArchiveError(Err::BadValue, "scale must be > 0");
                e.transform = t;
            }
            if (mask & 2) { scene3d::RenderDesc d; d.meshAsset = r.u64(); d.textureAsset = r.u64(); d.material = r.u32(); e.render = d; }
            if (mask & 4) { Velocity v; v.value = detail3d::rV3(r); e.velocity = v; }
            if (mask & 8) { Spin sp; sp.speed_y = r.f32Finite(); e.spin = sp; }
            if (mask & 16) {
                Camera c; c.position = detail3d::rV3(r); c.yaw = r.f32Finite(); c.pitch = r.f32Finite(); c.speed = r.f32Finite(); c.sensitivity = r.f32Finite();
                if (std::fabs(c.pitch) > detail3d::kHalfPi + 1e-3f) throw ArchiveError(Err::BadValue, "camera pitch out of range");
                if (c.speed < 0 || c.sensitivity < 0) throw ArchiveError(Err::BadValue, "negative camera speed/sensitivity");
                e.camera = c;
            }
            s.entities.push_back(std::move(e));
        }
        return s;
    }
};

namespace scene3d {
struct ComponentIds { int transform = 0, render = 1, velocity = 2, spin = 3, camera = 4; };   // runtime ids from addComponentType

// What the renderer has loaded: pointer <-> persistent asset id (ids must be non-zero).
struct AssetTable {
    std::vector<std::pair<uint64_t, const Mesh*>> meshes;
    std::vector<std::pair<uint64_t, const Texture*>> textures;
    uint64_t idOf(const Mesh* p) const { return find(meshes, p); }
    uint64_t idOf(const Texture* p) const { return find(textures, p); }
private:
    template <class V, class P> static uint64_t find(const V& v, P p) {
        if (!p) return 0;
        for (const auto& kv : v) if (kv.second == p) return kv.first;
        throw ArchiveError(Err::BadReference, "renderer uses an asset that is not in the asset table");
    }
};

// Owns the meshes/textures created by a load; MeshRenderer pointers point into it (node-based map:
// element addresses are stable). Keep it alive as long as the entities that use it; do not copy it.
struct LoadedAssets {
    std::unordered_map<uint64_t, Mesh> meshes;
    std::unordered_map<uint64_t, Texture> textures;
};
struct SceneBundle { SceneDesc scene; LoadedAssets assets; };
enum class LoadMode { IntoEmpty, ReplaceAll };

inline bool hasAny(entityManager& em, const ComponentIds& ids, int e) {
    return em.hasComponent(e, ids.transform) || em.hasComponent(e, ids.render) || em.hasComponent(e, ids.velocity) ||
           em.hasComponent(e, ids.spin) || em.hasComponent(e, ids.camera);
}

// Reads the ECS into a plain description (entities with no component are not saved).
inline SceneDesc capture(entityManager& em, const ComponentIds& ids, const AssetTable& assets) {
    SceneDesc s;
    int top = std::min(em.entityCount(), static_cast<int>(MAX_ENTITIES) - 1);      // never index past the storage arrays
    for (int id = 1; id <= top; ++id) {
        if (!hasAny(em, ids, id)) continue;
        EntityDesc d; d.id = static_cast<uint32_t>(id);
        if (em.hasComponent(id, ids.transform)) d.transform = *em.getComponent<Transform>(id, ids.transform);
        if (em.hasComponent(id, ids.render)) {
            const MeshRenderer* mr = em.getComponent<MeshRenderer>(id, ids.render);
            if (mr->material < 0) throw ArchiveError(Err::BadValue, "negative material index");
            d.render = RenderDesc{assets.idOf(mr->mesh), assets.idOf(mr->texture), static_cast<uint32_t>(mr->material)};
        }
        if (em.hasComponent(id, ids.velocity)) d.velocity = *em.getComponent<Velocity>(id, ids.velocity);
        if (em.hasComponent(id, ids.spin)) d.spin = *em.getComponent<Spin>(id, ids.spin);
        if (em.hasComponent(id, ids.camera)) d.camera = *em.getComponent<Camera>(id, ids.camera);
        s.entities.push_back(d);
    }
    return s;
}

// Adds the scene asset + only the meshes/textures it references (each once, even if shared).
inline void saveScene(ArchiveWriter& aw, uint64_t sceneId, entityManager& em, const ComponentIds& ids, const AssetTable& assets) {
    SceneDesc s = capture(em, ids, assets);
    std::set<uint64_t> usedM, usedT;
    for (const auto& e : s.entities) if (e.render) { if (e.render->meshAsset) usedM.insert(e.render->meshAsset); if (e.render->textureAsset) usedT.insert(e.render->textureAsset); }
    aw.add(sceneId, s);
    for (const auto& [id, p] : assets.meshes) if (usedM.count(id)) aw.add(id, *p);
    for (const auto& [id, p] : assets.textures) if (usedT.count(id)) aw.add(id, *p);
}

// Loads the scene, then ONLY the assets it references. Nothing touches the ECS yet.
inline SceneBundle loadScene(ArchiveReader& ar, uint64_t sceneId) {
    SceneBundle b;
    b.scene = ar.load<SceneDesc>(sceneId);
    for (const auto& e : b.scene.entities) {
        if (!e.render) continue;
        uint64_t m = e.render->meshAsset, t = e.render->textureAsset;
        if (m && !b.assets.meshes.count(m)) {
            if (!ar.find(m)) throw ArchiveError(Err::BadReference, "scene references missing mesh asset " + std::to_string(m));
            b.assets.meshes.emplace(m, ar.load<Mesh>(m));
        }
        if (t && !b.assets.textures.count(t)) {
            if (!ar.find(t)) throw ArchiveError(Err::BadReference, "scene references missing texture asset " + std::to_string(t));
            b.assets.textures.emplace(t, ar.load<Texture>(t));
        }
    }
    return b;
}

// Rebuilds entities + components (and pointers). All checks happen BEFORE the ECS is modified, so a
// failure leaves it untouched. Returns the new runtime id of each saved entity, in file order.
// ReplaceAll destroys the entities that own at least one component. Capacity is computed
// conservatively: ids already recycled by earlier destroyEntity calls are not counted as free.
inline std::vector<Entity> instantiate(const SceneDesc& s, entityManager& em, const ComponentIds& ids, const LoadedAssets& assets,
                                       int materialCount, LoadMode mode) {
    for (const auto& e : s.entities) {
        if (!e.render) continue;
        if (e.render->meshAsset && !assets.meshes.count(e.render->meshAsset)) throw ArchiveError(Err::BadReference, "unknown mesh asset");
        if (e.render->textureAsset && !assets.textures.count(e.render->textureAsset)) throw ArchiveError(Err::BadReference, "unknown texture asset");
        if (e.render->material >= static_cast<uint32_t>(std::max(materialCount, 0))) throw ArchiveError(Err::BadValue, "material index out of range");
    }
    std::vector<Entity> live;
    int top = std::min(em.entityCount(), static_cast<int>(MAX_ENTITIES) - 1);
    for (int id = 1; id <= top; ++id) if (hasAny(em, ids, id)) live.push_back(id);
    if (mode == LoadMode::IntoEmpty && em.entityCount() != 0) throw ArchiveError(Err::InvalidState, "target ECS is not empty");
    long freeSlots = (static_cast<long>(MAX_ENTITIES) - 1) - em.entityCount() + (mode == LoadMode::ReplaceAll ? static_cast<long>(live.size()) : 0);
    if (static_cast<long>(s.entities.size()) > freeSlots) throw ArchiveError(Err::Capacity, "scene has more entities than the ECS can hold");

    if (mode == LoadMode::ReplaceAll) for (Entity id : live) em.destroyEntity(id);
    std::vector<Entity> out;
    out.reserve(s.entities.size());
    for (const auto& d : s.entities) {
        Entity ne = em.createEntity();
        if (d.transform) { Transform t = *d.transform; em.addComponent(ne, ids.transform, &t); }
        if (d.render) {
            MeshRenderer mr;
            mr.mesh = d.render->meshAsset ? &assets.meshes.at(d.render->meshAsset) : nullptr;
            mr.texture = d.render->textureAsset ? &assets.textures.at(d.render->textureAsset) : nullptr;
            mr.material = static_cast<int>(d.render->material);
            em.addComponent(ne, ids.render, &mr);
        }
        if (d.velocity) { Velocity v = *d.velocity; em.addComponent(ne, ids.velocity, &v); }
        if (d.spin) { Spin sp = *d.spin; em.addComponent(ne, ids.spin, &sp); }
        if (d.camera) { Camera c = *d.camera; em.addComponent(ne, ids.camera, &c); }
        out.push_back(ne);
    }
    return out;
}
} // namespace scene3d
} // namespace asset
