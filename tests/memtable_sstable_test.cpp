#include <cassert>
#include <filesystem>
#include <string>

#include "storage/memtable/memtable.hpp"
#include "storage/sstable/sstable_reader.hpp"

int main() {
  const std::filesystem::path directory = "memtable_sstable_test_data";
  std::filesystem::remove_all(directory);

  {
    storage::Memtable memtable(directory);
    for (size_t i = 0; i < 64UL * 1024UL; ++i) {
      protocol::Request request{protocol::CommandType::SET,
                                "key-" + std::to_string(i),
                                "value-" + std::to_string(i)};
      assert(memtable.SET(request).ok);
      if (i == 10) {
        protocol::Request deleted{protocol::CommandType::DEL, "key-10", {}};
        assert(memtable.DEL(deleted).ok);
      }
    }
  }

  const auto path = directory / "0.sst";
  assert(std::filesystem::exists(path));

  storage::sstable::Reader reader(path);
  std::string error;
  if (!reader.Open(&error)) {
    assert(false && "failed to open generated SSTable");
  }

  auto result = reader.Get("key-11", &error);
  assert(result.ok && result.value == "value-11");

  result = reader.Get("key-10", &error);
  assert(!result.ok);

  std::filesystem::remove_all(directory);
  return 0;
}
