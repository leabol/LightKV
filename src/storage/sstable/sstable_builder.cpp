#include "storage/sstable/sstable_builder.hpp"

#include <fstream>
#include <limits>
#include <utility>
#include <vector>

#include "util/crc32.hpp"

namespace storage::sstable {
namespace {

constexpr uint32_t kFooterSize = 12;
constexpr uint32_t kBlockHeaderSize = 9;

void PutU32(std::string* output, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    output->push_back(static_cast<char>((value >> shift) & 0xff));
  }
}

void PutU64(std::string* output, uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    output->push_back(static_cast<char>((value >> shift) & 0xff));
  }
}

bool FitsU32(size_t value) {
  return value <= std::numeric_limits<uint32_t>::max();
}

bool WriteBytes(std::ofstream* file, const std::string& bytes) {
  file->write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return file->good();
}

std::string EncodeEntry(const data_entry& entry) {
  std::string result;
  result.reserve(9 + entry.key.size() + entry.value.size());
  PutU32(&result, static_cast<uint32_t>(entry.key.size()));
  PutU32(&result, static_cast<uint32_t>(entry.value.size()));
  result.push_back(static_cast<char>(entry.type));
  result.append(entry.key);
  result.append(entry.value);
  return result;
}

bool WriteDataBlock(std::ofstream* file, std::string* payload,
                    std::string* first_key, std::vector<index_entry>* index,
                    std::string* error) {
  if (payload->empty()) return true;

  const uint64_t offset = static_cast<uint64_t>(file->tellp());
  if (!FitsU32(payload->size())) {
    if (error != nullptr) *error = "SSTable block is too large";
    return false;
  }

  std::string header;
  header.reserve(kBlockHeaderSize);
  PutU32(&header, static_cast<uint32_t>(payload->size()));
  PutU32(&header, util::crc32::Value(payload->data(), payload->size()));
  header.push_back(static_cast<char>(compression_type::none));

  if (!WriteBytes(file, header) || !WriteBytes(file, *payload)) {
    if (error != nullptr) *error = "failed to write data block";
    return false;
  }

  const uint64_t block_size = kBlockHeaderSize + payload->size();
  if (block_size > std::numeric_limits<uint32_t>::max()) {
    if (error != nullptr) *error = "SSTable block is too large";
    return false;
  }
  index->push_back({*first_key, offset, static_cast<uint32_t>(block_size)});
  payload->clear();
  first_key->clear();
  return true;
}

bool ValidateEntry(const std::string& key, const data_entry& entry,
                   std::string* error) {
  if (entry.key != key) {
    if (error != nullptr) *error = "entry key does not match map key";
    return false;
  }
  if (!FitsU32(entry.key.size()) || !FitsU32(entry.value.size())) {
    if (error != nullptr) *error = "key or value is too large";
    return false;
  }
  if (entry.type == value_type::deletion && !entry.value.empty()) {
    if (error != nullptr) *error = "deletion entry must have an empty value";
    return false;
  }
  return true;
}

bool WriteIndexAndFooter(std::ofstream* file, const std::vector<index_entry>& index,
                         std::string* error) {
  const uint64_t index_offset = static_cast<uint64_t>(file->tellp());
  std::string index_bytes;
  index_bytes.reserve(4 + index.size() * 16);
  PutU32(&index_bytes, static_cast<uint32_t>(index.size()));
  for (const auto& item : index) {
    if (!FitsU32(item.first_key.size())) {
      if (error != nullptr) *error = "index key is too large";
      return false;
    }
    PutU32(&index_bytes, static_cast<uint32_t>(item.first_key.size()));
    index_bytes.append(item.first_key);
    PutU64(&index_bytes, item.block_offset);
    PutU32(&index_bytes, item.block_size);
  }
  if (!FitsU32(index_bytes.size()) || !WriteBytes(file, index_bytes)) {
    if (error != nullptr) *error = "failed to write index block";
    return false;
  }

  std::string footer_bytes;
  footer_bytes.reserve(kFooterSize);
  PutU64(&footer_bytes, index_offset);
  PutU32(&footer_bytes, static_cast<uint32_t>(index_bytes.size()));
  if (!WriteBytes(file, footer_bytes)) {
    if (error != nullptr) *error = "failed to write SSTable footer";
    return false;
  }
  return true;
}

}  // namespace

Builder::Builder(std::filesystem::path path, uint32_t block_payload_limit)
    : path_(std::move(path)), block_payload_limit_(block_payload_limit) {}

bool Builder::Build(const std::map<std::string, data_entry>& entries,
                    std::string* error) {
  if (block_payload_limit_ == 0) {
    if (error != nullptr) *error = "block payload limit must not be zero";
    return false;
  }

  const auto parent = path_.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
  std::ofstream file(path_, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    if (error != nullptr) *error = "failed to open SSTable for writing";
    return false;
  }

  std::vector<index_entry> index;
  std::string payload;
  std::string first_key;

  for (const auto& [key, entry] : entries) {
    if (!ValidateEntry(key, entry, error)) return false;

    std::string encoded = EncodeEntry(entry);
    if (encoded.size() > block_payload_limit_ && !payload.empty() &&
        !WriteDataBlock(&file, &payload, &first_key, &index, error)) {
      return false;
    }
    if (payload.empty()) first_key = key;
    payload.append(encoded);
  }
  if (!WriteDataBlock(&file, &payload, &first_key, &index, error) ||
      !WriteIndexAndFooter(&file, index, error)) {
    return false;
  }
  file.flush();
  return file.good();
}

}  // namespace storage::sstable
