// Tests of the Moteur3DCPU scene adapter. Build with the include path of <Moteur3DCPU>/header.
#include <cstdio>
#include <sstream>
#include "asset/adapters/scene3d.hpp"
using namespace asset;
using namespace asset::scene3d;

static int g_run = 0, g_fail = 0;
#define CHECK(c) do { ++g_run; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
template <class F> bool throwsCode(F&& f, Err c) { try { f(); } catch (const ArchiveError& e) { return e.code() == c; } return false; }
template <class F> bool throwsAny(F&& f) { try { f(); } catch (const ArchiveError&) { return true; } return false; }
static std::string toBytes(const ArchiveWriter& w) { std::ostringstream os; w.write(os); return os.str(); }

static bool sameMesh(const Mesh& a, const Mesh& b) {
    if (a.verts.size() != b.verts.size() || a.uvs.size() != b.uvs.size() || a.faces != b.faces || a.face_uvs != b.face_uvs) return false;
    for (size_t i = 0; i < a.verts.size(); ++i) if (a.verts[i].x != b.verts[i].x || a.verts[i].y != b.verts[i].y || a.verts[i].z != b.verts[i].z) return false;
    for (size_t i = 0; i < a.uvs.size(); ++i) if (a.uvs[i].x != b.uvs[i].x || a.uvs[i].y != b.uvs[i].y) return false;
    return true;
}
static ComponentIds registerNormal(entityManager& em) {
    ComponentIds i; i.transform = em.addComponentType<Transform>(); i.render = em.addComponentType<MeshRenderer>();
    i.velocity = em.addComponentType<Velocity>(); i.spin = em.addComponentType<Spin>(); i.camera = em.addComponentType<Camera>(); return i;
}
static ComponentIds registerShuffled(entityManager& em) {     // different registration order => different runtime ids
    ComponentIds i; i.camera = em.addComponentType<Camera>(); i.spin = em.addComponentType<Spin>(); i.velocity = em.addComponentType<Velocity>();
    i.render = em.addComponentType<MeshRenderer>(); i.transform = em.addComponentType<Transform>(); return i;
}
static int liveCount(entityManager& em, const ComponentIds& ids) { int n = 0; for (int id = 1; id <= em.entityCount(); ++id) n += hasAny(em, ids, id); return n; }

struct Scene3D {      // same shape as main.cpp: camera + two entities sharing ONE mesh and ONE texture
    entityManager em; ComponentIds ids; Mesh mesh; Texture tex; AssetTable table; int cam = 0, e1 = 0, e2 = 0;
    Scene3D() {
        ids = registerNormal(em);
        mesh.verts = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}; mesh.uvs = {{0, 0}, {1, 0}, {1, 1}};
        mesh.faces = {0, 1, 2, 0, 2, 3}; mesh.face_uvs = {0, 1, 2, 0, 2, -1};
        tex.width = 3; tex.height = 2; tex.pixels = {0xFF112233u, 0xFF445566u, 0xFF778899u, 0x80AABBCCu, 0xFFDDEEFFu, 0xFF000000u};
        table.meshes = {{10, &mesh}}; table.textures = {{20, &tex}};
        cam = em.createEntity("Camera"); e1 = em.createEntity(); e2 = em.createEntity();
        Camera c; c.position = {1, 2, 3}; c.yaw = 0.5f; c.pitch = -0.25f; c.sensitivity = 0.0025f; em.addComponent(cam, ids.camera, &c);
        MeshRenderer mr; mr.mesh = &mesh; mr.texture = &tex; mr.material = 0;
        Transform t1; t1.position = {-0.5f, 0, 0}; t1.scale = 0.5f; Spin s1; s1.speed_y = 0.7f;
        em.addComponent(e1, ids.transform, &t1); em.addComponent(e1, ids.render, &mr); em.addComponent(e1, ids.spin, &s1);
        Transform t2; t2.position = {-0.3f, 0, 1}; t2.scale = 0.5f; t2.rotation_y = 1.25f; Spin s2; s2.speed_y = 1.5f; Velocity v2; v2.value = {0, 1, 0};
        mr.material = 3;
        em.addComponent(e2, ids.transform, &t2); em.addComponent(e2, ids.render, &mr); em.addComponent(e2, ids.spin, &s2); em.addComponent(e2, ids.velocity, &v2);
    }
};
static std::string sceneBytes(Scene3D& w) { ArchiveWriter aw; saveScene(aw, 1, w.em, w.ids, w.table); return toBytes(aw); }

int main() {
    // ---- mesh / texture assets
    {
        Scene3D w; ArchiveWriter aw; aw.add(1, w.mesh); aw.add(2, w.tex); aw.add(3, Mesh{}); 
        std::string b = toBytes(aw); std::istringstream is(b); ArchiveReader ar(is);
        CHECK(sameMesh(ar.load<Mesh>(1), w.mesh));
        Texture t = ar.load<Texture>(2); CHECK(t.width == 3 && t.height == 2 && t.pixels == w.tex.pixels);
        Mesh empty = ar.load<Mesh>(3); CHECK(empty.verts.empty() && empty.faces.empty());
        Mesh bad = w.mesh; bad.faces[2] = 9; ArchiveWriter aw2; CHECK(throwsCode([&] { aw2.add(1, bad); }, Err::BadReference));
        Mesh bad2 = w.mesh; bad2.faces.pop_back(); CHECK(throwsCode([&] { ArchiveWriter x; x.add(1, bad2); }, Err::BadValue));
    }
    // ---- full scene round trip; shared assets stored once; component ids not persisted
    {
        Scene3D w; std::string b = sceneBytes(w); std::istringstream is(b); ArchiveReader ar(is);
        CHECK(ar.entries().size() == 3);                                      // scene + 1 mesh + 1 texture (shared by 2 entities)
        SceneBundle bundle = loadScene(ar, 1);
        entityManager em2; ComponentIds ids2 = registerShuffled(em2);
        std::vector<Entity> ne = instantiate(bundle.scene, em2, ids2, bundle.assets, 5, LoadMode::IntoEmpty);
        CHECK(ne.size() == 3 && liveCount(em2, ids2) == 3);
        Camera* c = em2.getComponent<Camera>(ne[0], ids2.camera);
        CHECK(c && c->position.x == 1 && c->position.z == 3 && c->yaw == 0.5f && c->pitch == -0.25f && c->sensitivity == 0.0025f && c->speed == 2.f);
        Transform* t2 = em2.getComponent<Transform>(ne[2], ids2.transform);
        CHECK(t2 && t2->position.z == 1 && t2->rotation_y == 1.25f && t2->scale == 0.5f);
        CHECK(em2.getComponent<Spin>(ne[1], ids2.spin)->speed_y == 0.7f && em2.getComponent<Velocity>(ne[2], ids2.velocity)->value.y == 1);
        CHECK(!em2.hasComponent(ne[1], ids2.velocity));
        MeshRenderer* r1 = em2.getComponent<MeshRenderer>(ne[1], ids2.render); MeshRenderer* r2 = em2.getComponent<MeshRenderer>(ne[2], ids2.render);
        CHECK(r1 && r2 && r1->mesh == r2->mesh && r1->texture == r2->texture && r1->mesh != &w.mesh);   // pointers rebuilt, still shared
        CHECK(sameMesh(*r1->mesh, w.mesh) && r1->texture->pixels == w.tex.pixels && r1->material == 0 && r2->material == 3);
    }
    // ---- ReplaceAll, null pointers, only referenced assets are written
    {
        Scene3D w; Mesh unused = w.mesh; w.table.meshes.push_back({11, &unused});     // not referenced => not saved
        entityManager em; ComponentIds ids = registerNormal(em);
        int a = em.createEntity(); int b2 = em.createEntity(); MeshRenderer mr; Transform t; t.position = {9, 9, 9};
        em.addComponent(a, ids.render, &mr); em.addComponent(b2, ids.transform, &t);   // mr has null mesh/texture
        ArchiveWriter aw; saveScene(aw, 5, em, ids, w.table); std::string bytes = toBytes(aw);
        std::istringstream is(bytes); ArchiveReader ar(is); CHECK(ar.entries().size() == 1);   // only the scene
        SceneBundle bundle = loadScene(ar, 5);
        Scene3D w2;                                                                    // existing world with 3 entities
        auto ne = instantiate(bundle.scene, w2.em, w2.ids, bundle.assets, 1, LoadMode::ReplaceAll);
        CHECK(ne.size() == 2 && liveCount(w2.em, w2.ids) == 2);
        CHECK(w2.em.getComponent<MeshRenderer>(ne[0], w2.ids.render)->mesh == nullptr);
        CHECK(w2.em.getComponent<Transform>(ne[1], w2.ids.transform)->position.x == 9);
        CHECK(throwsCode([&] { Scene3D w3; instantiate(bundle.scene, w3.em, w3.ids, bundle.assets, 1, LoadMode::IntoEmpty); }, Err::InvalidState));
    }
    // ---- failures leave the ECS untouched
    {
        Scene3D w; std::string b = sceneBytes(w); std::istringstream is(b); ArchiveReader ar(is);
        SceneBundle bundle = loadScene(ar, 1);
        Scene3D target; int before = liveCount(target.em, target.ids);
        float x0 = target.em.getComponent<Transform>(target.e1, target.ids.transform)->position.x;
        SceneBundle badMat = loadScene(ar, 1); badMat.scene.entities[2].render->material = 99;
        CHECK(throwsCode([&] { instantiate(badMat.scene, target.em, target.ids, badMat.assets, 5, LoadMode::ReplaceAll); }, Err::BadValue));
        SceneBundle badRef = loadScene(ar, 1); badRef.assets.meshes.clear();
        CHECK(throwsCode([&] { instantiate(badRef.scene, target.em, target.ids, badRef.assets, 5, LoadMode::ReplaceAll); }, Err::BadReference));
        SceneDesc big; for (uint32_t i = 1; i <= 120; ++i) { EntityDesc d; d.id = i; d.spin = Spin{}; big.entities.push_back(d); }
        CHECK(throwsCode([&] { instantiate(big, target.em, target.ids, bundle.assets, 5, LoadMode::ReplaceAll); }, Err::Capacity));
        CHECK(liveCount(target.em, target.ids) == before && target.em.getComponent<Transform>(target.e1, target.ids.transform)->position.x == x0);
        SceneDesc ok; for (uint32_t i = 1; i <= 99; ++i) { EntityDesc d; d.id = i; d.spin = Spin{}; ok.entities.push_back(d); }
        entityManager em99; ComponentIds i99 = registerNormal(em99);
        CHECK(instantiate(ok, em99, i99, bundle.assets, 5, LoadMode::IntoEmpty).size() == 99);       // exactly the capacity
        // scene whose mesh asset is missing from the archive
        ArchiveWriter aw; aw.add(1, bundle.scene); std::string b2 = toBytes(aw); std::istringstream is2(b2); ArchiveReader ar2(is2);
        CHECK(throwsCode([&] { loadScene(ar2, 1); }, Err::BadReference));
        // capture: pointer unknown to the table / negative material
        Scene3D w4; w4.table.meshes.clear(); CHECK(throwsCode([&] { capture(w4.em, w4.ids, w4.table); }, Err::BadReference));
        Scene3D w5; w5.em.getComponent<MeshRenderer>(w5.e1, w5.ids.render)->material = -1; CHECK(throwsCode([&] { capture(w5.em, w5.ids, w5.table); }, Err::BadValue));
    }
    // ---- hostile payloads with a VALID checksum (the CRC does not protect against a crafted file)
    {
        auto loadAs = [&](uint32_t type, ByteWriter& p, auto fn) {
            ArchiveWriter aw; aw.addRaw(1, type, 1, p.take()); std::string b = toBytes(aw); std::istringstream is(b); ArchiveReader ar(is); fn(ar);
        };
        { ByteWriter p; p.u32(0); p.u32(0); p.u32(3); for (int i = 0; i < 3; ++i) { p.u32(5); p.u32(0); }
          loadAs(200, p, [&](ArchiveReader& ar) { CHECK(throwsCode([&] { ar.load<Mesh>(1); }, Err::BadReference)); }); }          // index out of range
        { ByteWriter p; p.u32(0xFFFFFFFFu); loadAs(200, p, [&](ArchiveReader& ar) { CHECK(throwsCode([&] { ar.load<Mesh>(1); }, Err::BadSize)); }); }
        { ByteWriter p; p.u32(0); p.u32(0); p.u32(4); loadAs(200, p, [&](ArchiveReader& ar) { CHECK(throwsAny([&] { ar.load<Mesh>(1); })); }); }  // 4 corners: not multiple of 3 / truncated
        { ByteWriter p; p.u32(0); p.u32(4); loadAs(201, p, [&](ArchiveReader& ar) { CHECK(throwsCode([&] { ar.load<Texture>(1); }, Err::BadValue)); }); }
        { ByteWriter p; p.u32(16384); p.u32(16384); loadAs(201, p, [&](ArchiveReader& ar) { CHECK(throwsCode([&] { ar.load<Texture>(1); }, Err::BadSize)); }); }   // 1 GiB claimed, 8 B present
        { ByteWriter p; p.u32(20000); p.u32(2); loadAs(201, p, [&](ArchiveReader& ar) { CHECK(throwsCode([&] { ar.load<Texture>(1); }, Err::BadValue)); }); }
        { ByteWriter p; p.u32(0xFFFFFFFFu); loadAs(202, p, [&](ArchiveReader& ar) { CHECK(throwsCode([&] { ar.load<SceneDesc>(1); }, Err::BadSize)); }); }
        { ByteWriter p; p.u32(2); p.u32(5); p.u8(0); p.u32(5); p.u8(0); loadAs(202, p, [&](ArchiveReader& ar) { CHECK(throwsCode([&] { ar.load<SceneDesc>(1); }, Err::Duplicate)); }); }
        { ByteWriter p; p.u32(1); p.u32(5); p.u8(0x20); loadAs(202, p, [&](ArchiveReader& ar) { CHECK(throwsCode([&] { ar.load<SceneDesc>(1); }, Err::BadValue)); }); }
        { ByteWriter p; p.u32(1); p.u32(0); p.u8(0); loadAs(202, p, [&](ArchiveReader& ar) { CHECK(throwsCode([&] { ar.load<SceneDesc>(1); }, Err::BadValue)); }); }      // id 0
        { ByteWriter p; p.u32(1); p.u32(7); p.u8(1); p.f32(0); p.f32(std::nanf("")); p.f32(0); p.f32(0); p.f32(1);
          loadAs(202, p, [&](ArchiveReader& ar) { CHECK(throwsCode([&] { ar.load<SceneDesc>(1); }, Err::BadValue)); }); }                                          // NaN
        { ByteWriter p; p.u32(1); p.u32(7); p.u8(1); p.f32(0); p.f32(0); p.f32(0); p.f32(0); p.f32(0);
          loadAs(202, p, [&](ArchiveReader& ar) { CHECK(throwsCode([&] { ar.load<SceneDesc>(1); }, Err::BadValue)); }); }                                          // scale 0
        { ByteWriter p; p.u32(1); p.u32(7); p.u8(16); for (int i = 0; i < 3; ++i) p.f32(0); p.f32(0); p.f32(3.0f); p.f32(1); p.f32(1);
          loadAs(202, p, [&](ArchiveReader& ar) { CHECK(throwsCode([&] { ar.load<SceneDesc>(1); }, Err::BadValue)); }); }                                          // pitch 3 rad
    }
    // ---- truncation at every length, altered byte at every position
    {
        Scene3D w; std::string good = sceneBytes(w);
        bool allTrunc = true, allFlip = true;
        for (size_t n = 0; n < good.size(); ++n) { std::istringstream is(good.substr(0, n)); allTrunc &= throwsAny([&] { ArchiveReader x(is); }); }
        for (size_t i = 0; i < good.size(); ++i) {
            std::string bad = good; bad[i] ^= 0x01; std::istringstream is(bad);
            allFlip &= throwsAny([&] { ArchiveReader x(is); for (const Entry& e : x.entries()) x.readRaw(e.id); });
        }
        CHECK(allTrunc); CHECK(allFlip);
    }
    std::printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
