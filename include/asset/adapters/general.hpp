// Adapters for two general-purpose (non-physics) types. Adding a type = adding a Codec
// specialisation like these; archive.hpp / archive.cpp are never modified.
#pragma once
#include <string>
#include <vector>
#include "asset/archive.hpp"

namespace asset {

// ---- Person: schema v1 {name, age}  ->  v2 adds {level} ----
struct Person {
    std::string name;
    uint32_t age = 0;
    uint32_t level = 1;     // added in v2; default 1 when migrating from v1
};
template <> struct Codec<Person> {
    static constexpr uint32_t kType = 1;
    static constexpr uint16_t kVersion = 2;
    static void write(ByteWriter& w, const Person& p) { w.str(p.name); w.u32(p.age); w.u32(p.level); }
    static Person read(ByteReader& r, uint16_t v) {
        Person p;
        p.name = r.str();
        p.age = r.u32();
        p.level = (v >= 2) ? r.u32() : 1;     // migration: default value for the added field
        return p;
    }
};

// Legacy writer kept so that v1 archives can be produced (same type id, schema 1).
struct PersonV1 { std::string name; uint32_t age = 0; };
template <> struct Codec<PersonV1> {
    static constexpr uint32_t kType = 1;
    static constexpr uint16_t kVersion = 1;
    static void write(ByteWriter& w, const PersonV1& p) { w.str(p.name); w.u32(p.age); }
    static PersonV1 read(ByteReader& r, uint16_t) { PersonV1 p; p.name = r.str(); p.age = r.u32(); return p; }
};

// ---- Playlist: a string + a collection of composite structures ----
struct Track { std::string title; uint32_t durationMs = 0; };
struct Playlist { std::string title; std::vector<Track> tracks; };
template <> struct Codec<Playlist> {
    static constexpr uint32_t kType = 2;
    static constexpr uint16_t kVersion = 1;
    static void write(ByteWriter& w, const Playlist& p) {
        w.str(p.title);
        w.count(p.tracks.size());
        for (const Track& t : p.tracks) { w.str(t.title); w.u32(t.durationMs); }
    }
    static Playlist read(ByteReader& r, uint16_t) {
        Playlist p;
        p.title = r.str();
        uint32_t n = r.count(8);                // each track >= 4 (len) + 4 (duration) bytes
        p.tracks.reserve(n);                    // safe: n is bounded by the remaining bytes
        for (uint32_t i = 0; i < n; ++i) { Track t; t.title = r.str(); t.durationMs = r.u32(); p.tracks.push_back(std::move(t)); }
        return p;
    }
};

} // namespace asset
