#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// Test-only format-v1 serialization, independent of the production writer and
// its byte-at-a-time bit packer. Keep expected values for scalar query oracles.
namespace packed_fixture {
inline void little_endian(std::string& out, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) out.push_back(char(value >> (8 * i)));
}
struct Fixture {
    std::string payload;
    std::vector<int64_t> values;

    // Rebuild offsets/lengths after payload mutations so malformed tests reach
    // the decoder rather than failing footer validation.
    std::string file() const {
        std::string out = "MINICOL1";
        little_endian(out, 1, 4);
        little_endian(out, values.size(), 4);
        out += payload;
        std::string footer;
        little_endian(footer, values.size(), 8);
        little_endian(footer, 1, 4); // columns
        little_endian(footer, 1, 4); // groups
        little_endian(footer, 1, 4); // field name length
        footer += "v";
        footer += char(0); // int64
        little_endian(footer, values.size(), 4);
        footer += char(3); // bit packed
        little_endian(footer, 16, 8);
        little_endian(footer, payload.size(), 8);
        const auto bounds = std::minmax_element(values.begin(), values.end());
        little_endian(footer, uint64_t(*bounds.first), 8);
        little_endian(footer, uint64_t(*bounds.second), 8);
        out += footer;
        little_endian(out, footer.size(), 8);
        return out + "MCOLEND1";
    }
};
inline Fixture make(unsigned width, uint32_t rows, int64_t base) {
    if (width > 64 || !rows) throw std::runtime_error("invalid packed test dimensions");
    Fixture result;
    little_endian(result.payload, uint64_t(base), 8);
    result.payload += char(width);
    result.payload.resize(9 + (uint64_t(rows) * width + 7) / 8, char(0));
    const uint64_t mask = width == 0 ? 0 : UINT64_MAX >> (64 - width);
    for (uint32_t row = 0; row < rows; ++row) {
        uint64_t delta;
        switch (row % 6) {
        case 0: delta = 0; break;
        case 1: delta = mask; break;
        case 2: delta = UINT64_C(0xaaaaaaaaaaaaaaaa) & mask; break;
        case 3: delta = UINT64_C(0x5555555555555555) & mask; break;
        case 4: delta = width ? UINT64_C(1) << (row % width) : 0; break;
        default: delta = (uint64_t(row) * UINT64_C(0x9e3779b97f4a7c15)) & mask;
        }
        const uint64_t bits = uint64_t(base) + delta;
        // Defined conversion without implementation-defined unsigned-to-signed
        // casts or the production decoder's memcpy reinterpretation.
        result.values.push_back(bits <= uint64_t(INT64_MAX) ? int64_t(bits)
            : INT64_MIN + int64_t(bits - (UINT64_C(1) << 63)));
        for (unsigned bit = 0; bit < width; ++bit) {
            if (!(delta & (UINT64_C(1) << bit))) continue;
            const uint64_t position = uint64_t(row) * width + bit;
            auto& byte = result.payload[9 + size_t(position / 8)];
            byte = char(static_cast<unsigned char>(byte) | (1u << (position % 8)));
        }
    }
    return result;
}
} // namespace packed_fixture
