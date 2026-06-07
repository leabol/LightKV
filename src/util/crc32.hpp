#pragma once
#include <cstdint>
#include <cstddef>

namespace util {
namespace crc32 {

// 预先生成查表用的表函数
inline void GenerateTable(uint32_t table[256]) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            if (c & 1) c = 0xedb88320L ^ (c >> 1);
            else       c = c >> 1;
        }
        table[i] = c;
    }
}

// 计算 CRC32 的核心函数
inline uint32_t Value(const char* data, size_t size, uint32_t crc = 0) {
    // 第一次调用可以优化把 Table 生成缓存在单例或静态变量里
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        GenerateTable(table);
        init = true;
    }

    crc = crc ^ 0xffffffff;
    for (size_t i = 0; i < size; i++) {
        crc = table[(crc ^ static_cast<uint8_t>(data[i])) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xffffffff;
}

} // namespace crc32
} // namespace util
