#include "record/row.h"

/**
 * TODO: Student Implement
 */
uint32_t Row::SerializeTo(char *buf, Schema *schema) const {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(schema->GetColumnCount() == fields_.size(), "Fields size do not match schema's column size.");

  uint32_t field_count = fields_.size();
  if (field_count == 0) {
    return 0;
  }

  char *p = buf;

  //先写字段个数-fieldnums
  MACH_WRITE_UINT32(p, field_count);
  p += sizeof(uint32_t);

  //nullbitmap
  //fieldcount 个字段,每8个字段一个字节
  uint32_t bitmap_bytes = (field_count + 7) / 8;
  char *bitmap = p;
  //清0bitmap
  memset(bitmap, 0, bitmap_bytes);

  //写bitmap,如果字段为null - 1
  for (uint32_t i = 0; i < field_count; i++) {
    if (fields_[i]->IsNull()) {
      bitmap[i / 8] |= (1 << (i % 8));
    }
  }
  p += bitmap_bytes;

  //最后写字段数据,只有非null字段才写数据
  for (uint32_t i = 0; i < field_count; i++) {
    if (!fields_[i]->IsNull()) {
      p += fields_[i]->SerializeTo(p);
    }
  }

  return p - buf;
}

uint32_t Row::DeserializeFrom(char *buf, Schema *schema) {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(fields_.empty(), "Non empty field in row.");

  uint32_t off = 0;

  //读fieldcount
  uint32_t field_count = MACH_READ_UINT32(buf + off);
  off += sizeof(uint32_t);

  //读bitmap
  uint32_t bitmap_bytes = (field_count + 7) / 8;
  char *bitmap = buf + off;
  off += bitmap_bytes;

  //还原字段
  for (uint32_t i = 0; i < field_count; i++) {
    //判断null
    bool is_null = (bitmap[i / 8] >> (i % 8)) & 1;
    TypeId type = schema->GetColumn(i)->GetType();
    Field *field = nullptr;

    //写回字段数据,如果是null字段就不写数据
    off += Field::DeserializeFrom(buf + off, type, &field, is_null);
    fields_.push_back(field);
  }

  return off;
}

uint32_t Row::GetSerializedSize(Schema *schema) const {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(schema->GetColumnCount() == fields_.size(), "Fields size do not match schema's column size.");

  //空row返回0
  uint32_t field_count = fields_.size();
  if (field_count == 0) {
    return 0;
  }
  //fieldnums bitmap
  uint32_t size = sizeof(uint32_t);
  size += (field_count + 7) / 8;

  //非null字段数据
  for (auto field : fields_) {
    if (!field->IsNull()) {
      size += field->GetSerializedSize();
    }
  }
  return size;
}

void Row::GetKeyFromRow(const Schema *schema, const Schema *key_schema, Row &key_row) {
  auto columns = key_schema->GetColumns();
  std::vector<Field> fields;
  uint32_t idx;
  for (auto column : columns) {
    schema->GetColumnIndex(column->GetName(), idx);
    fields.emplace_back(*this->GetField(idx));
  }
  key_row = Row(fields);
}
