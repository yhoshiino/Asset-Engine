#include <cstdio>
#include <sstream>
#include "asset/adapters/general.hpp"
#include "asset/adapters/physics.hpp"
using namespace asset;
using namespace asset::physics;

static int g_run = 0, g_fail = 0;
#define CHECK(c) do { ++g_run; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
template <class F> bool throwsCode(F&& f, Err c) { try { f(); } catch (const ArchiveError& e) { return e.code() == c; } return false; }
template <class F> bool throwsAny(F&& f) { try { f(); } catch (const ArchiveError&) { return true; } return false; }

static std::string toBytes(const ArchiveWriter& w) { std::ostringstream os; w.write(os); return os.str(); }
static void fixFooter(std::string& s, size_t nEntries) {   // recompute the footer CRC (header + directory only) after a deliberate edit
    const uint8_t* d = reinterpret_cast<const uint8_t*>(s.data());
    size_t dirStart = s.size() - kFooterSize - nEntries * kEntrySize;
    uint32_t c = crc32(std::span<const uint8_t>(d + dirStart, nEntries * kEntrySize), crc32(std::span<const uint8_t>(d, kHeaderSize)));
    for (int i = 0; i < 4; ++i) s[s.size() - 4 + i] = static_cast<char>(c >> (8 * i));
}
static void put64(std::string& s, size_t pos, uint64_t v) { for (int i = 0; i < 8; ++i) s[pos + i] = static_cast<char>(v >> (8 * i)); }

static World makeWorld() {
    World w; w.gravity = {0, -9.81f, 0}; w.fixedTimestep = 1.f / 120.f;
    Body g; g.id = 1; g.isStatic = true; g.collider = {ColliderKind::Box, 0, {50, 1, 50}};
    Body a; a.id = 2; a.mass = 2; a.inertia = {0.5f, 0.5f, 0.5f}; a.position = {0, 5, 0}; a.linearVelocity = {1, 0, 0};
    a.angularVelocity = {0, 2, 0}; a.collider = {ColliderKind::Sphere, 0.5f, {}};
    Body b = a; b.id = 3; b.position = {2, 5, 0}; b.orientation = {0, 0.7071068f, 0, 0.7071068f};
    w.bodies = {g, a, b}; w.constraints = {{2, 3, 1.5f}};
    w.rebuildDerived();
    return w;
}
static bool sameBody(const Body& a, const Body& b) {
    return a.id == b.id && a.isStatic == b.isStatic && a.position.x == b.position.x && a.position.y == b.position.y &&
           a.orientation.y == b.orientation.y && a.orientation.w == b.orientation.w && a.mass == b.mass &&
           a.inertia.x == b.inertia.x && a.linearVelocity.x == b.linearVelocity.x && a.angularVelocity.y == b.angularVelocity.y &&
           a.collider.kind == b.collider.kind && a.collider.radius == b.collider.radius &&
           a.collider.halfExtents.x == b.collider.halfExtents.x && a.invMass == b.invMass;
}
static std::string worldArchive(const World& w) { ArchiveWriter aw; aw.add(10, w); return toBytes(aw); }

int main() {
    // --- encoding primitives
    { ByteWriter w; w.u32(0x01020304); CHECK(w.bytes()[0] == 4 && w.bytes()[3] == 1); }                 // little-endian
    { const uint8_t s[] = {'1','2','3','4','5','6','7','8','9'}; CHECK(crc32(s) == 0xCBF43926u); }      // standard CRC-32 check value
    { ByteWriter w; w.f32(-1.5f); w.f64(3.25); w.boolean(true); w.str("héllo €");
      ByteReader r(w.bytes()); CHECK(r.f32() == -1.5f && r.f64() == 3.25 && r.boolean() && r.str() == "héllo €"); r.expectEnd(); }
    { ByteWriter w; w.u32(2); w.u8(0xC3); w.u8(0x28); ByteReader r(w.bytes()); CHECK(throwsCode([&] { r.str(); }, Err::BadValue)); }  // invalid UTF-8

    // --- general round trips (Person + Playlist in one archive), access by id
    {
        ArchiveWriter aw;
        Person p{"Ada Lovelace", 36, 7};
        Playlist pl{"Road trip", {{"Song A", 180000}, {"Chanson é", 201000}}};
        aw.add(100, p); aw.add(200, pl);
        std::string bytes = toBytes(aw);
        std::istringstream is(bytes); ArchiveReader ar(is);
        CHECK(ar.entries().size() == 2);
        Playlist pl2 = ar.load<Playlist>(200);                      // loaded without touching asset 100
        CHECK(pl2.title == pl.title && pl2.tracks.size() == 2 && pl2.tracks[1].title == pl.tracks[1].title && pl2.tracks[1].durationMs == 201000);
        Person p2 = ar.load<Person>(100);
        CHECK(p2.name == p.name && p2.age == 36 && p2.level == 7);
        CHECK(throwsCode([&] { ar.load<Person>(200); }, Err::BadType));
        CHECK(throwsCode([&] { ar.load<Person>(999); }, Err::NotFound));
    }
    // --- empty archive, empty collections
    {
        ArchiveWriter aw; std::string b = toBytes(aw); std::istringstream is(b); ArchiveReader ar(is);
        CHECK(ar.entries().empty() && b.size() == kHeaderSize + kFooterSize);
        ArchiveWriter aw2; aw2.add(1, Playlist{}); aw2.add(2, World{});
        std::string b2 = toBytes(aw2); std::istringstream is2(b2); ArchiveReader ar2(is2);
        Playlist pl = ar2.load<Playlist>(1); CHECK(pl.title.empty() && pl.tracks.empty());
        CHECK(ar2.load<World>(2).bodies.empty());
    }
    // --- migration v1 -> v2, unknown versions
    {
        ArchiveWriter aw; aw.add(1, PersonV1{"Old", 40});
        std::string b = toBytes(aw); std::istringstream is(b); ArchiveReader ar(is);
        Person p = ar.load<Person>(1); CHECK(p.name == "Old" && p.age == 40 && p.level == 1);  // default for the added field
        ArchiveWriter aw3; aw3.addRaw(1, 1, 3, {});       // schema 3 does not exist yet
        std::string b3 = toBytes(aw3); std::istringstream is3(b3); ArchiveReader ar3(is3);
        CHECK(throwsCode([&] { ar3.load<Person>(1); }, Err::UnsupportedVersion));
        std::string bv = toBytes(aw); bv[4] = 2;           // format version 2
        std::istringstream isv(bv); CHECK(throwsCode([&] { ArchiveReader x(isv); }, Err::UnsupportedVersion));
        std::string bm = toBytes(aw); bm[0] = 'X';
        std::istringstream ism(bm); CHECK(throwsCode([&] { ArchiveReader x(ism); }, Err::BadMagic));
    }
    // --- truncation at EVERY length, and an altered byte at EVERY position
    {
        ArchiveWriter aw; aw.add(1, Person{"Bob", 20, 2}); aw.add(2, Playlist{"P", {{"t", 1}}}); aw.add(3, makeWorld());
        std::string good = toBytes(aw);
        bool allTrunc = true;
        for (size_t n = 0; n < good.size(); ++n) {
            std::istringstream is(good.substr(0, n));
            allTrunc &= throwsAny([&] { ArchiveReader x(is); });
        }
        CHECK(allTrunc);
        bool allFlip = true;
        for (size_t i = 0; i < good.size(); ++i) {
            std::string bad = good; bad[i] ^= 0x01;
            std::istringstream is(bad);
            bool rejected = throwsAny([&] {
                ArchiveReader x(is);
                for (const Entry& e : x.entries()) x.readRaw(e.id);    // payload CRCs are checked on access
            });
            if (!rejected) { allFlip = false; std::printf("byte %zu not detected\n", i); }
        }
        CHECK(allFlip);
    }
    // --- excessive sizes (valid CRC, hostile content): rejected BEFORE allocating
    {
        ByteWriter w; w.u32(0); w.u32(0xFFFFFFFFu);                        // empty title, 4 billion tracks
        ArchiveWriter aw; aw.addRaw(1, 2, 1, w.take());
        std::string b = toBytes(aw); std::istringstream is(b); ArchiveReader ar(is);
        CHECK(throwsCode([&] { ar.load<Playlist>(1); }, Err::BadSize));
        ByteWriter w2; w2.u32(0xFFFFFFF0u);                                // string longer than the asset
        ArchiveWriter aw2; aw2.addRaw(1, 2, 1, w2.take());
        std::string b2 = toBytes(aw2); std::istringstream is2(b2); ArchiveReader ar2(is2);
        CHECK(throwsCode([&] { ar2.load<Playlist>(1); }, Err::Truncated));
        // directory entry with a gigantic size, footer CRC fixed up
        ArchiveWriter aw3; aw3.add(1, Person{"A", 1, 1});
        std::string b3 = toBytes(aw3); size_t dir = b3.size() - kFooterSize - kEntrySize;
        put64(b3, dir + 8 + 4 + 2 + 2 + 8, 0xFFFFFFFFFFFFFFF0ull); fixFooter(b3, 1);
        std::istringstream is3(b3); CHECK(throwsCode([&] { ArchiveReader x(is3); }, Err::BadOffset));
        // trailing garbage inside an asset
        ArchiveWriter aw4; ByteWriter p; p.str("x"); p.u32(1); p.u32(1); p.u8(0);
        aw4.addRaw(1, 1, 2, p.take()); std::string b4 = toBytes(aw4); std::istringstream is4(b4); ArchiveReader ar4(is4);
        CHECK(throwsCode([&] { ar4.load<Person>(1); }, Err::TrailingBytes));
    }
    // --- duplicates
    {
        ArchiveWriter aw; aw.add(1, Person{}); CHECK(throwsCode([&] { aw.add(1, Person{}); }, Err::Duplicate));
        ArchiveWriter aw2; aw2.add(1, Person{}); aw2.add(2, Person{});
        std::string b = toBytes(aw2); size_t dir = b.size() - kFooterSize - 2 * kEntrySize;
        put64(b, dir + kEntrySize, 1); fixFooter(b, 2);                      // second id := first id
        std::istringstream is(b); CHECK(throwsCode([&] { ArchiveReader x(is); }, Err::Duplicate));
    }
    // --- physics: scene round trip, snapshot, replace world, rejection without partial state
    {
        World w = makeWorld();
        std::string b = worldArchive(w); std::istringstream is(b); ArchiveReader ar(is);
        World l = ar.load<World>(10);
        CHECK(l.bodies.size() == 3 && l.constraints.size() == 1 && l.fixedTimestep == w.fixedTimestep && l.gravity.y == w.gravity.y);
        for (size_t i = 0; i < 3; ++i) CHECK(sameBody(w.bodies[i], l.bodies[i]));
        CHECK(l.bodyIndex.at(3) == 2 && l.bodies[1].invMass == 0.5f && l.bodies[0].invMass == 0.f);   // derived data rebuilt
        Snapshot s; s.stepIndex = 4242; s.simTime = 35.35; s.world = w;
        ArchiveWriter aw; aw.add(1, s); std::string bs = toBytes(aw); std::istringstream iss(bs); ArchiveReader ars(iss);
        Snapshot s2 = ars.load<Snapshot>(1); CHECK(s2.stepIndex == 4242 && s2.simTime == 35.35 && s2.world.bodies.size() == 3);
        World target = makeWorld(); target.bodies.pop_back(); target.rebuildDerived();
        target = ar.load<World>(10);                                      // replace an existing world
        CHECK(target.bodies.size() == 3);

        auto expectRejected = [&](World bad, Err code) {
            std::string bb = worldArchive(bad); std::istringstream isb(bb); ArchiveReader arb(isb);
            World keep = makeWorld(); size_t before = keep.bodies.size();
            bool ok = false;
            try { keep = arb.load<World>(10); } catch (const ArchiveError& e) { ok = e.code() == code; }
            CHECK(ok && keep.bodies.size() == before);                    // target untouched on failure
        };
        World badRef = makeWorld(); badRef.constraints.push_back({2, 99, 1.f}); expectRejected(badRef, Err::BadReference);
        World dupId = makeWorld(); dupId.bodies[2].id = 2; expectRejected(dupId, Err::Duplicate);
        World nan = makeWorld(); nan.bodies[1].position.x = std::nanf(""); expectRejected(nan, Err::BadValue);
        World zeroMass = makeWorld(); zeroMass.bodies[1].mass = 0; expectRejected(zeroMass, Err::BadValue);
        World badQ = makeWorld(); badQ.bodies[1].orientation = {0, 0, 0, 3}; expectRejected(badQ, Err::BadValue);
        World badDt = makeWorld(); badDt.fixedTimestep = 0; expectRejected(badDt, Err::BadValue);
    }
    // --- write failure
    {
        ArchiveWriter aw; aw.add(1, Person{"x", 1, 1});
        std::ostringstream os; os.setstate(std::ios::badbit);
        CHECK(throwsCode([&] { aw.write(os); }, Err::Io));
        CHECK(throwsCode([&] { aw.save("/nonexistent_dir_xyz/a.asen"); }, Err::Io));
    }
    std::printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
