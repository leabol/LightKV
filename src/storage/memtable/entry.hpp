#pragma once

#include <stdint.h>
#include <string>

struct data_header {
  uint32_t key_size;
  uint32_t value_size;
};
struct data_block {
  struct data_header header;
  std::string key;
  std::string value;
};
struct index_block {
  std::string key;
  uint64_t offset;
  uint32_t data_size;
};
struct index_header {
  uint32_t index_count;
};
struct block{
  struct data_block data;
  struct index_block index;
};