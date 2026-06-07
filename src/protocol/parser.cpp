#include "protocol/parser.hpp"

namespace protocol {

bool parserRequest(net::Buffer& buffer, Request& req) {
  // Need at least 4 bytes for total length field
  uint8_t* header = buffer.peek(4);
  if (!header)
    return false;

  uint32_t totalSize = read_u32(header);
  const uint32_t minTotal = 4 + 1 + 2 + 3;  // len + op + key_len + val_len
  if (totalSize < minTotal)
    return false;

  if (totalSize > static_cast<uint32_t>(buffer.maxSize()))
    return false;

  // Wait for full frame
  if (totalSize > buffer.readableByte())
    return false;

  uint8_t* data = buffer.readPayload(totalSize);
  if (!data)
    return false;

  const uint32_t payload_len = totalSize - 4;  // excludes the 4-byte length
  if (payload_len < 1 + 2 + 3)
    return false;

  uint8_t op = data[0];
  if (op > static_cast<uint8_t>(CommandType::DEL))
    return false;
  req.cmd = static_cast<CommandType>(op);

  size_t key_len = read_u16(data + 1);
  // Ensure key_len fits before reading val_len
  if (static_cast<uint32_t>(1 + 2 + key_len + 3) > payload_len)
    return false;

  size_t val_len = read_u24(data + 3 + key_len);

  // Total payload must exactly match expected layout
  if (static_cast<uint32_t>(1 + 2 + key_len + 3 + val_len) != payload_len)
    return false;

  req.key = std::string(reinterpret_cast<char*>(data + 3), key_len);
  req.value = std::string(reinterpret_cast<char*>(data + 3 + key_len + 3), val_len);

  return true;
}

}  // namespace protocol