#include "asset/archive.hpp"
#include <fstream>
#include <istream>
#include <ostream>

namespace asset {

void ArchiveWriter::addRaw(uint64_t id, uint32_t type, uint16_t schema, std::vector<uint8_t> payload) {
    if (type == 0) throw ArchiveError(Err::BadType, "type 0 is reserved");
    if (schema == 0) throw ArchiveError(Err::UnsupportedVersion, "schema version 0 is invalid");
    if (!ids_.emplace(id, true).second) throw ArchiveError(Err::Duplicate, "duplicate asset id " + std::to_string(id));
    Pending p;
    p.e.id = id; p.e.type = type; p.e.schema = schema;
    p.data = std::move(payload);
    items_.push_back(std::move(p));
}

void ArchiveWriter::write(std::ostream& os) const {
    uint64_t off = kHeaderSize;
    ByteWriter dir;
    dir.reserve(items_.size() * kEntrySize);
    for (const auto& it : items_) {
        dir.u64(it.e.id); dir.u32(it.e.type); dir.u16(it.e.schema); dir.u16(0);
        dir.u64(off); dir.u64(it.data.size()); dir.u32(crc32(it.data));
        off += it.data.size();
    }
    ByteWriter hdr;
    for (char c : kMagic) hdr.u8(static_cast<uint8_t>(c));
    hdr.u16(kFormatVersion); hdr.u16(0);
    hdr.count(items_.size());
    hdr.u64(off);                                   // directory offset = end of payloads

    auto put = [&](const std::vector<uint8_t>& v) {
        os.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size()));
    };
    put(hdr.bytes());
    for (const auto& it : items_) put(it.data);
    put(dir.bytes());
    ByteWriter foot;
    foot.u32(crc32(dir.bytes(), crc32(hdr.bytes())));
    put(foot.bytes());
    os.flush();
    if (!os) throw ArchiveError(Err::Io, "write failed");
}

void ArchiveWriter::save(const std::filesystem::path& p) const {
    std::filesystem::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw ArchiveError(Err::Io, "cannot open " + tmp.string());
        write(f);
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    if (ec) throw ArchiveError(Err::Io, "rename failed: " + ec.message());
}

namespace {
std::vector<uint8_t> readAt(std::istream& is, uint64_t off, uint64_t n) {
    if (n == 0) return {};                               // gcount() would be stale for a 0-byte read
    std::vector<uint8_t> buf(static_cast<size_t>(n));    // callers have bounded n by the file size
    is.clear();
    is.seekg(static_cast<std::streamoff>(off));
    is.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n));
    if (!is || static_cast<uint64_t>(is.gcount()) != n) throw ArchiveError(Err::Truncated, "short read");
    return buf;
}
}

ArchiveReader::ArchiveReader(std::istream& is) : is_(is) {
    is_.clear();
    is_.seekg(0, std::ios::end);
    auto end = is_.tellg();
    if (end < 0) throw ArchiveError(Err::Io, "cannot determine size");
    fileSize_ = static_cast<uint64_t>(end);
    if (fileSize_ < kHeaderSize + kFooterSize) throw ArchiveError(Err::Truncated, "file smaller than header + footer");

    auto hdrBytes = readAt(is_, 0, kHeaderSize);
    ByteReader h(hdrBytes);
    for (char c : kMagic) if (h.u8() != static_cast<uint8_t>(c)) throw ArchiveError(Err::BadMagic, "bad magic number");
    uint16_t version = h.u16();
    if (version != kFormatVersion) throw ArchiveError(Err::UnsupportedVersion, "unsupported format version " + std::to_string(version));
    if (h.u16() != 0) throw ArchiveError(Err::BadValue, "unknown flags");
    uint32_t count = h.u32();
    uint64_t dirOff = h.u64();

    if (count > (fileSize_ - kHeaderSize - kFooterSize) / kEntrySize) throw ArchiveError(Err::BadSize, "entry count exceeds file size");
    if (dirOff < kHeaderSize || dirOff > fileSize_) throw ArchiveError(Err::BadOffset, "directory offset out of range");
    if (dirOff + uint64_t(count) * kEntrySize + kFooterSize != fileSize_) throw ArchiveError(Err::BadSize, "directory does not end at end of file");

    auto dirBytes = readAt(is_, dirOff, uint64_t(count) * kEntrySize);
    auto footBytes = readAt(is_, fileSize_ - kFooterSize, kFooterSize);
    uint32_t stored = ByteReader(footBytes).u32();
    if (crc32(dirBytes, crc32(hdrBytes)) != stored) throw ArchiveError(Err::BadChecksum, "header/directory checksum mismatch");

    ByteReader d(dirBytes);
    entries_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Entry e;
        e.id = d.u64(); e.type = d.u32(); e.schema = d.u16();
        if (d.u16() != 0) throw ArchiveError(Err::BadValue, "reserved field not zero");
        e.offset = d.u64(); e.size = d.u64(); e.crc = d.u32();
        if (e.type == 0) throw ArchiveError(Err::BadType, "entry with reserved type 0");
        if (e.schema == 0) throw ArchiveError(Err::UnsupportedVersion, "entry with schema 0");
        if (e.offset < kHeaderSize || e.offset > dirOff || e.size > dirOff - e.offset)   // overflow-safe form
            throw ArchiveError(Err::BadOffset, "entry range outside payload area");
        if (!index_.emplace(e.id, entries_.size()).second) throw ArchiveError(Err::Duplicate, "duplicate asset id in directory");
        entries_.push_back(e);
    }
}

const Entry* ArchiveReader::find(uint64_t id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &entries_[it->second];
}
const Entry& ArchiveReader::require(uint64_t id) const {
    const Entry* e = find(id);
    if (!e) throw ArchiveError(Err::NotFound, "no asset with id " + std::to_string(id));
    return *e;
}
std::vector<uint8_t> ArchiveReader::readRaw(uint64_t id) const {
    const Entry& e = require(id);
    auto bytes = readAt(is_, e.offset, e.size);
    if (crc32(bytes) != e.crc) throw ArchiveError(Err::BadChecksum, "payload checksum mismatch for asset " + std::to_string(id));
    return bytes;
}

} // namespace asset
