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

// 查表在首次调用时生成。用函数内 static 对象（C++11 的 magic static 保证
// 只初始化一次且线程安全），避免原来的 `static bool init` 在多线程下
// 出现「看到 init==true 但表还没填完」的数据竞争。
struct Crc32Table {
  uint32_t data[256];
  Crc32Table() { GenerateTable(data); }
};

inline const Crc32Table& GetTable() {
  static const Crc32Table table;
  return table;
}

// 计算 CRC32 的核心函数
inline uint32_t Value(const char* data, size_t size, uint32_t crc = 0) {
  const uint32_t* table = GetTable().data;
  crc = crc ^ 0xffffffff;
  for (size_t i = 0; i < size; i++) {
    crc = table[(crc ^ static_cast<uint8_t>(data[i])) & 0xff] ^ (crc >> 8);
  }
  return crc ^ 0xffffffff;
}

} // namespace crc32
} // namespace util
