// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320, init/xorout 0xFFFFFFFF), own implementation.
// Detects accidental corruption only; it is NOT authentication.
#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace asset {
inline uint32_t crc32(std::span<const uint8_t> data, uint32_t previous = 0) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = ~previous;                       // chainable: crc32(b, crc32(a)) == crc32(a+b)
    for (uint8_t b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return ~c;
}
} // namespace asset
