#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>

#include "storage/wal/wal_writer.hpp"
#include "storage/wal/wal_reader.hpp"
#include "storage/wal/log_record.hpp"
#include "storage/memtable/memtable.hpp"
#include "protocol/request.hpp"

static const char* WAL_PATH = "/tmp/lightkv_test_wal";

static void cleanup() {
  std::remove(WAL_PATH);
}

// 测试1: 写入后读回，验证数据一致
void test_write_read() {
  cleanup();
  std::cout << "=== Test 1: write and read ===" << std::endl;

  {
    wal::WALWriter writer(WAL_PATH);
    writer.append({wal::RecordType::SET, "key1", "value1"});
    writer.append({wal::RecordType::SET, "key2", "value2"});
    writer.append({wal::RecordType::DELETE, "key1", ""});
  }

  wal::WALReader reader(WAL_PATH);
  wal::LogRecord record;

  assert(reader.next(record));
  assert(record.type == wal::RecordType::SET);
  assert(record.key == "key1");
  assert(record.value == "value1");

  assert(reader.next(record));
  assert(record.type == wal::RecordType::SET);
  assert(record.key == "key2");
  assert(record.value == "value2");

  assert(reader.next(record));
  assert(record.type == wal::RecordType::DELETE);
  assert(record.key == "key1");
  assert(record.value == "");

  assert(!reader.next(record));  // EOF

  std::cout << "PASSED" << std::endl;
  cleanup();
}

// 测试2: 从WAL恢复到Memtable
void test_recovery() {
  cleanup();
  std::cout << "=== Test 2: recovery ===" << std::endl;

  {
    wal::WALWriter writer(WAL_PATH);
    writer.append({wal::RecordType::SET, "a", "100"});
    writer.append({wal::RecordType::SET, "b", "200"});
    writer.append({wal::RecordType::SET, "c", "300"});
    writer.append({wal::RecordType::DELETE, "b", ""});
  }

  storage::Memtable memtable;
  wal::WALReader::Recover(WAL_PATH, memtable);

  protocol::Request req;

  req = {protocol::CommandType::GET, "a", ""};
  auto rsp = memtable.GET(req);
  assert(rsp.ok && rsp.value == "100");

  req = {protocol::CommandType::GET, "b", ""};
  rsp = memtable.GET(req);
  assert(!rsp.ok);  // 已删除

  req = {protocol::CommandType::GET, "c", ""};
  rsp = memtable.GET(req);
  assert(rsp.ok && rsp.value == "300");

  std::cout << "PASSED" << std::endl;
  cleanup();
}

// 测试3: 文件不存在时恢复不崩溃
void test_missing_file() {
  cleanup();
  std::cout << "=== Test 3: missing WAL file ===" << std::endl;

  storage::Memtable memtable;
  wal::WALReader::Recover("/tmp/lightkv_nonexistent_wal", memtable);

  std::cout << "PASSED" << std::endl;
}

// 测试4: 截断的记录应停止解析
void test_truncated_record() {
  cleanup();
  std::cout << "=== Test 4: truncated record ===" << std::endl;

  // 写一条完整记录
  {
    wal::WALWriter writer(WAL_PATH);
    writer.append({wal::RecordType::SET, "good", "ok"});
  }

  // 人为截断文件：写入不完整的第二条记录
  {
    int fd = ::open(WAL_PATH, O_WRONLY | O_APPEND);
    uint8_t partial[] = {0x01, 0x02, 0x03};  // 不完整的header
    ::write(fd, partial, sizeof(partial));
    ::close(fd);
  }

  // 读取：第一条成功，第二条（截断）返回false
  wal::WALReader reader(WAL_PATH);
  wal::LogRecord record;

  assert(reader.next(record));
  assert(record.key == "good");

  assert(!reader.next(record));  // 截断记录，应返回false

  std::cout << "PASSED" << std::endl;
  cleanup();
}

// 测试5: CRC损坏的记录应停止解析
void test_corrupted_crc() {
  cleanup();
  std::cout << "=== Test 5: corrupted CRC ===" << std::endl;

  // 写一条完整记录
  {
    wal::WALWriter writer(WAL_PATH);
    writer.append({wal::RecordType::SET, "test", "data"});
  }

  // 人为篡改CRC字段（文件前4字节）
  {
    int fd = ::open(WAL_PATH, O_WRONLY);
    uint8_t bad_crc[] = {0xFF, 0xFF, 0xFF, 0xFF};
    ::write(fd, bad_crc, sizeof(bad_crc));
    ::close(fd);
  }

  wal::WALReader reader(WAL_PATH);
  wal::LogRecord record;

  assert(!reader.next(record));  // CRC不匹配，应返回false

  std::cout << "PASSED" << std::endl;
  cleanup();
}

int main() {
  test_write_read();
  test_recovery();
  test_missing_file();
  test_truncated_record();
  test_corrupted_crc();
  std::cout << "\nAll WAL tests passed!" << std::endl;
  return 0;
}
