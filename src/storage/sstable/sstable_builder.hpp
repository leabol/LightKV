#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

#include "storage/sstable/sstable_format.hpp"

namespace storage::sstable {

class Builder {
 public:
  explicit Builder(std::filesystem::path path,
                   uint32_t block_payload_limit = 4096);

  Builder(const Builder&) = delete;
  Builder& operator=(const Builder&) = delete;

  // 将按 key 排序的记录写成一个 SSTable 文件。
  bool Build(const std::map<std::string, data_entry>& entries,
             std::string* error = nullptr);

 private:
  std::filesystem::path path_;
  uint32_t block_payload_limit_;
};

}  // namespace storage::sstable
