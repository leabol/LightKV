#pragma once

#include <cstdint>
#include <string>

namespace storage::sstable {

enum class value_type : uint8_t {
  value = 0,
  deletion = 1,
};

// 当前暂不支持压缩，保留该字段便于后续扩展。
enum class compression_type : uint8_t {
  none = 0,
};

// 一个数据条目的序列化信息。
// 该结构仅作为内存描述，写文件时应按字段逐个序列化，不能直接写入结构体。
struct data_header {
  uint32_t key_size;
  uint32_t value_size;
  value_type type;
};

struct data_entry {
  std::string key;
  std::string value;
  value_type type{value_type::value};
};

// 索引中记录每个数据块的起始 key 和文件位置。
struct index_entry {
  std::string first_key;
  uint64_t block_offset;
  uint32_t block_size;
};

struct index_header {
  uint32_t index_count;
};

// SSTable 文件尾部，用于定位索引块。
struct footer {
  uint64_t index_offset;
  uint32_t index_size;
};

struct block_header {
  // 数据块负载大小，不包含 block_header 本身。
  uint32_t data_size;
  uint32_t checksum;
  compression_type compression;
};

}  // namespace storage::sstable