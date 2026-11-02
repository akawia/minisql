#include "record/schema.h"

/**
 * TODO: Student Implement
 */
uint32_t Schema::SerializeTo(char *buf) const {
  char *p = buf;

  // 1. 魔数
  MACH_WRITE_UINT32(p, SCHEMA_MAGIC_NUM);
  p += sizeof(uint32_t);

  // 2. 列的个数
  MACH_WRITE_UINT32(p, GetColumnCount());
  p += sizeof(uint32_t);

  // 3. 逐个序列化每一列,累加它们各自写入的字节数
  for (auto column : columns_) {
    p += column->SerializeTo(p);
  }

  return p - buf;
}

uint32_t Schema::GetSerializedSize() const {
  uint32_t size = sizeof(uint32_t) + sizeof(uint32_t);  // magic + 列数
  for (auto column : columns_) {
    size += column->GetSerializedSize();
  }
  return size;
}

uint32_t Schema::DeserializeFrom(char *buf, Schema *&schema) {
  uint32_t off = 0;

  // 1. 读魔数校验
  uint32_t magic = MACH_READ_UINT32(buf + off);
  off += sizeof(uint32_t);
  ASSERT(magic == SCHEMA_MAGIC_NUM, "Invalid schema magic number.");

  // 2. 读列数
  uint32_t column_count = MACH_READ_UINT32(buf + off);
  off += sizeof(uint32_t);

  // 3. 逐个反序列化出 Column,收集成 vector
  std::vector<Column *> columns;
  for (uint32_t i = 0; i < column_count; i++) {
    Column *column = nullptr;
    off += Column::DeserializeFrom(buf + off, column);
    columns.push_back(column);
  }

  // 4. 由这些 Column 构造 Schema,is_manage_ = true 让 schema 负责析构它们
  schema = new Schema(columns, true);
  return off;
}