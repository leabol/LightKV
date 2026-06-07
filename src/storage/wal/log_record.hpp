#pragma once

#include <cstdint>
#include <string>
#include <stdexcept>

#include "protocol/request.hpp"

namespace wal {
enum class RecordType {
  SET,
  DELETE
};

inline RecordType FromCommandType(protocol::CommandType cmd) {
  switch (cmd) {
    case protocol::CommandType::SET: return RecordType::SET;
    case protocol::CommandType::DEL: return RecordType::DELETE;
    default: throw std::invalid_argument("GET command should not be written to WAL");
  }
}

struct LogRecord {
  RecordType type;

  std::string key;
  std::string value;
};

struct RecordHeader {
  uint32_t crc;
  uint32_t key_size;
  uint32_t value_size;
  uint8_t type;
};
}