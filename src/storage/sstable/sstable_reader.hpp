#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "protocol/response.hpp"

namespace storage::sstable {

struct LookupResult {
  bool found{false};
  bool deleted{false};
  std::string value;
};

class Reader {
 public:
  explicit Reader(std::filesystem::path path);

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  // 打开并读取 SSTable 的索引。
  bool Open(std::string* error = nullptr);

  // 查询 key；删除标记和不存在的 key 都返回 ok=false。
  protocol::Response Get(const std::string& key, std::string* error = nullptr);

  // 查询时区分“不存在”和删除标记，供多层存储合并查询使用。
  LookupResult Lookup(const std::string& key, std::string* error = nullptr);

 private:
  struct LoadedIndexEntry {
    std::string first_key;
    uint64_t block_offset;
    uint32_t block_size;
  };

  bool ReadBlock(uint64_t offset, uint32_t size, const std::string& key,
                 LookupResult* result, std::string* error);
  bool LoadIndex(uint64_t index_offset, uint32_t index_size,
                 std::string* error);

  std::filesystem::path path_;
  std::ifstream file_;
  std::vector<LoadedIndexEntry> index_;
};

}  // namespace storage::sstable
