// scene_demo save <out.asen> [mesh.obj texture.png]   builds the same scene as main.cpp (camera + 2 entities sharing a mesh)
// scene_demo load <in.asen>                           loads it into a fresh ECS and prints what was rebuilt
#include <fstream>
#include <iostream>
#include "asset/adapters/scene3d.hpp"
using namespace asset;
using namespace asset::scene3d;

static ComponentIds registerIds(entityManager& em) {
    ComponentIds i;
    i.transform = em.addComponentType<Transform>(); i.render = em.addComponentType<MeshRenderer>();
    i.velocity = em.addComponentType<Velocity>();   i.spin = em.addComponentType<Spin>(); i.camera = em.addComponentType<Camera>();
    return i;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "usage: scene_demo save <out> [mesh.obj tex.png] | load <in>\n"; return 2; }
    std::string cmd = argv[1];
    try {
        if (cmd == "save") {
            Mesh mesh; Texture tex;
            if (argc >= 5) { if (!mesh.load(argv[3]) || !tex.load(argv[4])) return 1; }
            else {                                                              // small procedural fallback
                mesh.verts = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}; mesh.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
                mesh.faces = {0, 1, 2, 0, 2, 3}; mesh.face_uvs = {0, 1, 2, 0, 2, 3};
                tex.width = 4; tex.height = 4; tex.pixels.assign(16, 0xFFCC8844u);
            }
            entityManager em; ComponentIds ids = registerIds(em);
            int camera = em.createEntity("Camera"), e1 = em.createEntity(), e2 = em.createEntity();
            Camera cam; cam.sensitivity = 0.0025f; em.addComponent(camera, ids.camera, &cam);
            MeshRenderer mr; mr.mesh = &mesh; mr.texture = &tex;
            Transform t1; t1.position = {-0.5f, 0, 0}; t1.scale = 0.5f; Spin s1; s1.speed_y = 0.7f;
            em.addComponent(e1, ids.transform, &t1); em.addComponent(e1, ids.render, &mr); em.addComponent(e1, ids.spin, &s1);
            Transform t2; t2.position = {-0.3f, 0, 1}; t2.scale = 0.5f; Spin s2; s2.speed_y = 1.5f;
            em.addComponent(e2, ids.transform, &t2); em.addComponent(e2, ids.render, &mr); em.addComponent(e2, ids.spin, &s2);

            AssetTable table; table.meshes = {{10, &mesh}}; table.textures = {{20, &tex}};   // stable asset ids chosen by us
            ArchiveWriter aw; saveScene(aw, 1, em, ids, table); aw.save(argv[2]);
            std::cout << "saved " << argv[2] << "\n";
        } else if (cmd == "load") {
            std::ifstream f(argv[2], std::ios::binary); ArchiveReader ar(f);
            for (const Entry& e : ar.entries()) std::cout << "asset " << e.id << " type " << e.type << " size " << e.size << "\n";
            SceneBundle b = loadScene(ar, 1);
            entityManager em; ComponentIds ids = registerIds(em);
            auto ne = instantiate(b.scene, em, ids, b.assets, /*materialCount*/ 5, LoadMode::IntoEmpty);
            for (Entity id : ne) {
                std::cout << "entity " << id << ":";
                if (em.hasComponent(id, ids.transform)) std::cout << " Transform(scale=" << em.getComponent<Transform>(id, ids.transform)->scale << ")";
                if (em.hasComponent(id, ids.render)) { auto* r = em.getComponent<MeshRenderer>(id, ids.render); std::cout << " MeshRenderer(" << (r->mesh ? r->mesh->face_count() : 0) << " faces)"; }
                if (em.hasComponent(id, ids.spin)) std::cout << " Spin(" << em.getComponent<Spin>(id, ids.spin)->speed_y << ")";
                if (em.hasComponent(id, ids.camera)) std::cout << " Camera";
                std::cout << "\n";
            }
        }
    } catch (const ArchiveError& e) { std::cerr << "archive error: " << e.what() << "\n"; return 1; }
    return 0;
}
