// Usage:  asset_demo write <dir>   -> writes general_v1.asen, general_v2.asen, scene.asen
//         asset_demo list  <file>  -> lists the directory of an archive
//         asset_demo load  <file>  -> loads every known asset type and prints it
#include <fstream>
#include <iostream>
#include "asset/adapters/general.hpp"
#include "asset/adapters/physics.hpp"
using namespace asset;

static void list(std::istream& is) {
    ArchiveReader ar(is);
    for (const Entry& e : ar.entries())
        std::cout << "id=" << e.id << " type=" << e.type << " schema=" << e.schema << " offset=" << e.offset
                  << " size=" << e.size << " crc=0x" << std::hex << e.crc << std::dec << "\n";
}

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "usage: asset_demo write <dir> | list <file> | load <file>\n"; return 2; }
    std::string cmd = argv[1], arg = argv[2];
    try {
        if (cmd == "write") {
            std::filesystem::path d = arg;
            std::filesystem::create_directories(d);
            ArchiveWriter v1; v1.add(1, PersonV1{"Ada", 36}); v1.add(2, Playlist{"Road trip", {{"Song A", 180000}}});
            v1.save(d / "general_v1.asen");
            ArchiveWriter v2; v2.add(1, Person{"Ada", 36, 7}); v2.add(2, Playlist{"Road trip", {{"Song A", 180000}, {"Song B", 200000}}});
            v2.save(d / "general_v2.asen");
            physics::World w;
            physics::Body ground; ground.id = 1; ground.isStatic = true; ground.collider = {physics::ColliderKind::Box, 0, {50, 1, 50}};
            physics::Body ball; ball.id = 2; ball.mass = 1; ball.inertia = {0.4f, 0.4f, 0.4f}; ball.position = {0, 10, 0};
            ball.collider = {physics::ColliderKind::Sphere, 0.5f, {}};
            w.bodies = {ground, ball}; w.rebuildDerived();
            ArchiveWriter sc; sc.add(1, w); sc.save(d / "scene.asen");
            std::cout << "wrote 3 archives in " << d << "\n";
        } else if (cmd == "list") {
            std::ifstream f(arg, std::ios::binary); list(f);
        } else if (cmd == "load") {
            std::ifstream f(arg, std::ios::binary); ArchiveReader ar(f);
            for (const Entry& e : ar.entries()) {
                if (e.type == Codec<Person>::kType) { auto p = ar.load<Person>(e.id); std::cout << "Person " << p.name << " age=" << p.age << " level=" << p.level << " (schema v" << e.schema << ")\n"; }
                else if (e.type == Codec<Playlist>::kType) { auto p = ar.load<Playlist>(e.id); std::cout << "Playlist '" << p.title << "' " << p.tracks.size() << " tracks\n"; }
                else if (e.type == Codec<physics::World>::kType) { auto w = ar.load<physics::World>(e.id); std::cout << "World " << w.bodies.size() << " bodies, dt=" << w.fixedTimestep << "\n"; }
            }
        }
    } catch (const ArchiveError& e) { std::cerr << "archive error: " << e.what() << "\n"; return 1; }
    return 0;
}
