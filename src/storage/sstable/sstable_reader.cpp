#include "storage/sstable/sstable_reader.hpp"

#include <algorithm>
#include <utility>

#include "storage/sstable/sstable_format.hpp"
#include "util/crc32.hpp"

namespace storage::sstable {
namespace {

constexpr uint32_t kFooterSize = 12;
constexpr uint32_t kBlockHeaderSize = 9;
constexpr uint32_t kEntryHeaderSize = 9;

bool ReadU32(const std::string& bytes, size_t* position, uint32_t* value) {
  if (*position > bytes.size() || bytes.size() - *position < 4) {
    return false;
  }
  *value = static_cast<uint8_t>(bytes[*position]) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[*position + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[*position + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[*position + 3])) << 24);
  *position += 4;
  return true;
}

bool ReadU64(const std::string& bytes, size_t* position, uint64_t* value) {
  if (*position > bytes.size() || bytes.size() - *position < 8) {
    return false;
  }
  *value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    *value |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[*position])) << shift;
    ++*position;
  }
  return true;
}

bool ReadFileRange(std::ifstream* file, uint64_t offset, uint32_t size,
                   std::string* output) {
  file->clear();
  file->seekg(static_cast<std::streamoff>(offset));
  if (!file->good()) {
    return false;
  }
  output->assign(size, '\0');
  file->read(output->data(), static_cast<std::streamsize>(size));
  return file->good() && file->gcount() == static_cast<std::streamsize>(size);
}

bool ReadFooter(std::ifstream* file, uint64_t file_size, uint64_t* index_offset,
                uint32_t* index_size, std::string* error) {
  std::string footer_bytes;
  if (!ReadFileRange(file, file_size - kFooterSize, kFooterSize,
                     &footer_bytes)) {
    if (error != nullptr) {
      *error = "failed to read SSTable footer";
    }
    return false;
  }
  size_t position = 0;
  if (!ReadU64(footer_bytes, &position, index_offset) ||
      !ReadU32(footer_bytes, &position, index_size) ||
      *index_offset > file_size - kFooterSize ||
      *index_size > file_size - kFooterSize - *index_offset) {
    if (error != nullptr) {
      *error = "invalid SSTable footer";
    }
    return false;
  }
  return true;
}

}  // namespace

bool Reader::LoadIndex(uint64_t index_offset, uint32_t index_size,
                       std::string* error) {
  std::string index_bytes;
  if (!ReadFileRange(&file_, index_offset, index_size, &index_bytes)) {
    if (error != nullptr) {
      *error = "failed to read SSTable index";
    }
    return false;
  }

  size_t position = 0;
  uint32_t index_count = 0;
  if (!ReadU32(index_bytes, &position, &index_count)) {
    if (error != nullptr) {
      *error = "invalid SSTable index header";
    }
    return false;
  }

  index_.clear();
  index_.reserve(index_count);
  for (uint32_t i = 0; i < index_count; ++i) {
    uint32_t key_size = 0;
    uint64_t block_offset = 0;
    uint32_t block_size = 0;
    if (!ReadU32(index_bytes, &position, &key_size) ||
        position > index_bytes.size() ||
        key_size > index_bytes.size() - position) {
      if (error != nullptr) {
        *error = "invalid SSTable index entry";
      }
      return false;
    }
    std::string first_key = index_bytes.substr(position, key_size);
    position += key_size;
    if (!ReadU64(index_bytes, &position, &block_offset) ||
        !ReadU32(index_bytes, &position, &block_size)) {
      if (error != nullptr) {
        *error = "invalid SSTable index entry";
      }
      return false;
    }
    index_.push_back({std::move(first_key), block_offset, block_size});
  }
  if (position != index_bytes.size()) {
    if (error != nullptr) {
      *error = "trailing bytes in SSTable index";
    }
    return false;
  }
  return true;
}

bool ReadBlockPayload(const std::string& block, std::string* payload,
                      std::string* error) {
  if (block.size() < kBlockHeaderSize) {
    if (error != nullptr) {
      *error = "invalid SSTable block size";
    }
    return false;
  }
  size_t position = 0;
  uint32_t data_size = 0;
  uint32_t checksum = 0;
  if (!ReadU32(block, &position, &data_size) ||
      !ReadU32(block, &position, &checksum) || position >= block.size()) {
    if (error != nullptr) {
      *error = "invalid SSTable block header";
    }
    return false;
  }
  const auto compression = static_cast<uint8_t>(block[position++]);
  if (compression != static_cast<uint8_t>(compression_type::none) ||
      data_size != block.size() - kBlockHeaderSize) {
    if (error != nullptr) {
      *error = "unsupported or invalid SSTable block";
    }
    return false;
  }
  *payload = block.substr(kBlockHeaderSize);
  if (util::crc32::Value(payload->data(), payload->size()) != checksum) {
    if (error != nullptr) {
      *error = "SSTable block checksum mismatch";
    }
    return false;
  }
  return true;
}

bool FindEntry(const std::string& payload, const std::string& key,
               LookupResult* result, std::string* error) {
  size_t position = 0;
  while (position < payload.size()) {
    uint32_t key_size = 0;
    uint32_t value_size = 0;
    if (payload.size() - position < kEntryHeaderSize ||
        !ReadU32(payload, &position, &key_size) ||
        !ReadU32(payload, &position, &value_size)) {
      if (error != nullptr) {
        *error = "invalid SSTable entry header";
      }
      return false;
    }
    const auto type = static_cast<value_type>(
        static_cast<uint8_t>(payload[position++]));
    const uint64_t entry_size = static_cast<uint64_t>(key_size) + value_size;
    if (entry_size > payload.size() - position) {
      if (error != nullptr) {
        *error = "invalid SSTable entry size";
      }
      return false;
    }
    std::string entry_key = payload.substr(position, key_size);
    position += key_size;
    std::string value = payload.substr(position, value_size);
    position += value_size;

    if (entry_key == key) {
      if (type == value_type::deletion) {
        result->found = true;
        result->deleted = true;
        return true;
      }
      if (type != value_type::value) {
        if (error != nullptr) {
          *error = "unknown SSTable value type";
        }
        return false;
      }
      result->found = true;
      result->value = std::move(value);
      return true;
    }
    if (entry_key > key) {
      return true;
    }
  }
  return true;
}

Reader::Reader(std::filesystem::path path) : path_(std::move(path)) {}

bool Reader::Open(std::string* error) {
  file_.close();
  file_.open(path_, std::ios::binary);
  if (!file_.is_open()) {
    if (error != nullptr) {
      *error = "failed to open SSTable";
    }
    return false;
  }

  file_.seekg(0, std::ios::end);
  const auto file_size = file_.tellg();
  if (file_size < static_cast<std::streamoff>(kFooterSize)) {
    if (error != nullptr) {
      *error = "SSTable is smaller than footer";
    }
    return false;
  }

  uint64_t index_offset = 0;
  uint32_t index_size = 0;
  return ReadFooter(&file_, static_cast<uint64_t>(file_size), &index_offset,
            &index_size, error) &&
      LoadIndex(index_offset, index_size, error);
}

protocol::Response Reader::Get(const std::string& key, std::string* error) {
  const auto result = Lookup(key, error);
  if (!result.found || result.deleted) {
    return {false, {}};
  }
  return {true, result.value};
}

LookupResult Reader::Lookup(const std::string& key, std::string* error) {
  LookupResult result;
  if (!file_.is_open()) {
    if (error != nullptr) {
      *error = "SSTable is not open";
    }
    return result;
  }
  if (index_.empty()) {
    return result;
  }

  auto it = std::upper_bound(
      index_.begin(), index_.end(), key,
      [](const std::string& value, const LoadedIndexEntry& item) {
        return value < item.first_key;
      });
  if (it == index_.begin()) {
    return result;
  }
  --it;
  ReadBlock(it->block_offset, it->block_size, key, &result, error);
  return result;
}

bool Reader::ReadBlock(uint64_t offset, uint32_t size, const std::string& key,
                       LookupResult* result, std::string* error) {
  if (size < kBlockHeaderSize) {
    if (error != nullptr) {
      *error = "invalid SSTable block size";
    }
    return false;
  }
  std::string block;
  if (!ReadFileRange(&file_, offset, size, &block)) {
    if (error != nullptr) {
      *error = "failed to read SSTable block";
    }
    return false;
  }

  std::string payload;
  return ReadBlockPayload(block, &payload, error) &&
      FindEntry(payload, key, result, error);
}

}  // namespace storage::sstable
