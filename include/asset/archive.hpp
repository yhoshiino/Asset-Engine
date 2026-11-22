// Archive format (all integers little-endian):
//
//   [0 .. 20)    Header   : magic "ASEN"(4) | formatVersion u16 | flags u16 (=0)
//                           | entryCount u32 | directoryOffset u64
//   [20 .. D)    Payloads : asset blobs, back to back
//   [D .. D+36n) Directory: n entries of
//                           assetId u64 | type u32 | schemaVersion u16 | reserved u16 (=0)
//                           | offset u64 | size u64 | payloadCrc32 u32
//   [end-4, end) Footer   : CRC-32 of the 20 header bytes followed by the directory bytes
//
// Coverage: header + directory are covered by the footer CRC, each payload by its
// own entry CRC => every byte of the file is covered by exactly one checksum.
#pragma once
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>
#include "asset/byte_io.hpp"
#include "asset/crc32.hpp"

namespace asset {

inline constexpr char kMagic[4] = {'A', 'S', 'E', 'N'};
inline constexpr uint16_t kFormatVersion = 1;
inline constexpr size_t kHeaderSize = 20, kEntrySize = 36, kFooterSize = 4;

struct Entry {
    uint64_t id = 0;
    uint32_t type = 0;      // 0 is reserved
    uint16_t schema = 0;    // version of the payload schema (0 invalid)
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t crc = 0;
};

// Adapter customisation point. Specialise for a type T:
//   static constexpr uint32_t kType; static constexpr uint16_t kVersion;
//   static void write(ByteWriter&, const T&);
//   static T read(ByteReader&, uint16_t schemaVersion);   // must handle every version <= kVersion
// The archive core never names a domain type.
template <class T> struct Codec;

class ArchiveWriter {
public:
    template <class T> void add(uint64_t id, const T& value) {
        ByteWriter w;
        Codec<T>::write(w, value);
        addRaw(id, Codec<T>::kType, Codec<T>::kVersion, w.take());   // payload is moved, not copied
    }
    void addRaw(uint64_t id, uint32_t type, uint16_t schema, std::vector<uint8_t> payload);
    void write(std::ostream& os) const;
    void save(const std::filesystem::path& p) const;   // writes p.tmp then renames: no half-written archive at p
private:
    struct Pending { Entry e; std::vector<uint8_t> data; };
    std::vector<Pending> items_;
    std::unordered_map<uint64_t, bool> ids_;
};

class ArchiveReader {
public:
    // Reads and validates ONLY header + directory (bounded by file size).
    explicit ArchiveReader(std::istream& is);
    const std::vector<Entry>& entries() const { return entries_; }
    const Entry* find(uint64_t id) const;
    std::vector<uint8_t> readRaw(uint64_t id) const;   // reads one payload, verifies its CRC

    template <class T> T load(uint64_t id) const {
        const Entry& e = require(id);
        if (e.type != Codec<T>::kType) throw ArchiveError(Err::BadType, "asset has a different type");
        if (e.schema == 0 || e.schema > Codec<T>::kVersion)
            throw ArchiveError(Err::UnsupportedVersion, "unsupported schema version " + std::to_string(e.schema));
        std::vector<uint8_t> bytes = readRaw(id);
        ByteReader r(bytes);
        T v = Codec<T>::read(r, e.schema);
        r.expectEnd();
        return v;   // built on the side: the caller never sees a partially loaded object
    }
private:
    const Entry& require(uint64_t id) const;
    std::istream& is_;
    uint64_t fileSize_ = 0;
    std::vector<Entry> entries_;
    std::unordered_map<uint64_t, size_t> index_;
};

} // namespace asset
