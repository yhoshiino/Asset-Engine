// Byte-level access: explicit little-endian encoding, bounds-checked reading.
// Never copies raw object memory: every field is written value by value.
#pragma once
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace asset {

enum class Err {
    Truncated, BadMagic, UnsupportedVersion, BadSize, BadOffset, Duplicate,
    BadChecksum, BadType, BadReference, BadValue, NotFound, Io, TrailingBytes,
    InvalidState, Capacity
};

class ArchiveError : public std::runtime_error {
public:
    ArchiveError(Err c, const std::string& msg) : std::runtime_error(msg), code_(c) {}
    Err code() const noexcept { return code_; }
private:
    Err code_;
};

inline bool validUtf8(std::string_view s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        if (c < 0x80) { ++i; continue; }
        size_t len; uint32_t cp;
        if ((c & 0xE0) == 0xC0)      { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k) {
            uint8_t cc = static_cast<uint8_t>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return false; // overlong
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

class ByteWriter {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { le(v, 2); }
    void u32(uint32_t v) { le(v, 4); }
    void u64(uint64_t v) { le(v, 8); }
    void boolean(bool b) { u8(b ? 1 : 0); }
    void f32(float f) { u32(std::bit_cast<uint32_t>(f)); }   // IEEE-754 binary32 assumed
    void f64(double f) { u64(std::bit_cast<uint64_t>(f)); }
    // Sequence length / string length: u32.
    void count(size_t n) {
        if (n > UINT32_MAX) throw ArchiveError(Err::BadSize, "sequence too long");
        u32(static_cast<uint32_t>(n));
    }
    void str(std::string_view s) {
        if (!validUtf8(s)) throw ArchiveError(Err::BadValue, "string is not valid UTF-8");
        count(s.size());
        for (char c : s) buf_.push_back(static_cast<uint8_t>(c));
    }
    void reserve(size_t n) { buf_.reserve(n); }
    const std::vector<uint8_t>& bytes() const { return buf_; }
    std::vector<uint8_t> take() { return std::move(buf_); }
private:
    void le(uint64_t v, int n) { for (int i = 0; i < n; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    std::vector<uint8_t> buf_;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const uint8_t> d) : d_(d) {}
    uint8_t u8() { return static_cast<uint8_t>(le(1)); }
    uint16_t u16() { return static_cast<uint16_t>(le(2)); }
    uint32_t u32() { return static_cast<uint32_t>(le(4)); }
    uint64_t u64() { return le(8); }
    bool boolean() {
        uint8_t v = u8();
        if (v > 1) throw ArchiveError(Err::BadValue, "invalid boolean byte");
        return v == 1;
    }
    float f32() { return std::bit_cast<float>(u32()); }
    double f64() { return std::bit_cast<double>(u64()); }
    float f32Finite() {
        float f = f32();
        if (!std::isfinite(f)) throw ArchiveError(Err::BadValue, "non-finite float");
        return f;
    }
    // Reads a length and checks that `n` elements of at least `minElemSize` bytes
    // can fit in what remains: BOUNDS THE ALLOCATION BEFORE ANYTHING IS RESERVED.
    uint32_t count(size_t minElemSize) {
        uint32_t n = u32();
        if (minElemSize && n > remaining() / minElemSize)
            throw ArchiveError(Err::BadSize, "element count exceeds remaining bytes");
        return n;
    }
    std::string str() {
        uint32_t n = u32();
        need(n);
        std::string s(reinterpret_cast<const char*>(d_.data() + pos_), n);
        pos_ += n;
        if (!validUtf8(s)) throw ArchiveError(Err::BadValue, "string is not valid UTF-8");
        return s;
    }
    size_t remaining() const { return d_.size() - pos_; }
    void expectEnd() const {
        if (remaining() != 0) throw ArchiveError(Err::TrailingBytes, "unread bytes at end of asset");
    }
private:
    void need(size_t n) const { if (n > remaining()) throw ArchiveError(Err::Truncated, "unexpected end of data"); }
    uint64_t le(int n) {
        need(n);
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(d_[pos_ + i]) << (8 * i);
        pos_ += n;
        return v;
    }
    std::span<const uint8_t> d_;
    size_t pos_ = 0;
};

} // namespace asset
