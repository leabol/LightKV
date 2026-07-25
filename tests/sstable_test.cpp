#include <cassert>
#include <filesystem>
#include <map>
#include <string>

#include "storage/sstable/sstable_builder.hpp"
#include "storage/sstable/sstable_reader.hpp"

int main() {
  const std::filesystem::path path = "sstable_test.sst";
  std::map<std::string, storage::sstable::data_entry> entries{
      {"a", {"a", "alpha", storage::sstable::value_type::value}},
      {"b", {"b", "", storage::sstable::value_type::value}},
      {"c", {"c", "", storage::sstable::value_type::deletion}},
      {"d", {"d", "delta", storage::sstable::value_type::value}},
  };

  std::string error;
  storage::sstable::Builder builder(path, 20);
  assert(builder.Build(entries, &error));

  storage::sstable::Reader reader(path);
  assert(reader.Open(&error));

  auto response = reader.Get("a", &error);
  assert(response.ok && response.value == "alpha");

  response = reader.Get("b", &error);
  assert(response.ok && response.value.empty());

  response = reader.Get("c", &error);
  assert(!response.ok);

  response = reader.Get("missing", &error);
  assert(!response.ok);

  response = reader.Get("d", &error);
  assert(response.ok && response.value == "delta");

  std::filesystem::remove(path);
  return 0;
}
