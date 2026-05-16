#pragma once

#include "response.hpp"
#include <cstdint>
#include <string>

// Request frame layout:
//  +----------+-------+----------+---------+----------+---------+
//  | 4-byte   | 1-byte| 2-byte   | key     | 3-byte   | value   |
//  | total    | op    | key_len  | bytes   | value_len| bytes   |
//  | length   |       |          |         |          |         |
//  +----------+-------+----------+---------+----------+---------+
//
// Response frame layout:
//  +----------+--------+----------+---------+
//  | 4-byte   | 1-byte | 3-byte   | value   |
//  | total    | status | value_len| bytes   |
//  | length   | code   |          |         |
//  +----------+--------+----------+---------+

namespace protocol{

// Helper functions for big-endian encoding
inline void write_u32_be(uint8_t* data, uint32_t val) {
    data[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    data[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    data[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    data[3] = static_cast<uint8_t>(val & 0xFF);
}

inline void write_u24_be(uint8_t* data, uint32_t val) {
    data[0] = static_cast<uint8_t>((val >> 16) & 0xFF);
    data[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    data[2] = static_cast<uint8_t>(val & 0xFF);
}

inline void write_u16_be(uint8_t* data, uint16_t val) {
    data[0] = static_cast<uint8_t>((val >> 8) & 0xFF);
    data[1] = static_cast<uint8_t>(val & 0xFF);
}

// Encode Response into a serialized byte string
// Status code: 0xC8 (200) for success, 0x00 for error
// Value is truncated to 3-byte length max
std::string encodeResponse(const Response& rep);

} // namespace protocol
