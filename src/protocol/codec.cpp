#include "codec.hpp"
#include <string>

namespace protocol {

std::string encodeResponse(const Response& rep) {
    const uint32_t maxValueLen = (1U << 24) - 1; // 3-byte max
    uint32_t val_len = rep.value.size();
    if (val_len > maxValueLen) {
        val_len = maxValueLen;
    }
    
    const uint32_t totalLen = 4 + 1 + 3 + val_len; // len + status + val_len + value
    
    std::string encoded;
    encoded.reserve(totalLen);
    
    // Encode header: [4-byte len][1-byte status][3-byte val_len]
    uint8_t header[8];
    write_u32_be(header + 0, totalLen);
    header[4] = rep.ok ? 0xC8 : 0x00; // 200 for ok, 0 for error
    write_u24_be(header + 5, val_len);
    
    encoded.append(reinterpret_cast<char*>(header), 8);
    if (val_len > 0) {
        encoded.append(rep.value, 0, val_len);
    }
    
    return encoded;
}

} // namespace protocol
