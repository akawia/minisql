#include "record/column.h"

#include "glog/logging.h"

Column::Column(std::string column_name, TypeId type, uint32_t index, bool nullable, bool unique)
    : name_(std::move(column_name)), type_(type), table_ind_(index), nullable_(nullable), unique_(unique) {
  ASSERT(type != TypeId::kTypeChar, "Wrong constructor for CHAR type.");
  switch (type) {
    case TypeId::kTypeInt:
      len_ = sizeof(int32_t);
      break;
    case TypeId::kTypeFloat:
      len_ = sizeof(float_t);
      break;
    default:
      ASSERT(false, "Unsupported column type.");
  }
}

Column::Column(std::string column_name, TypeId type, uint32_t length, uint32_t index, bool nullable, bool unique)
    : name_(std::move(column_name)),
      type_(type),
      len_(length),
      table_ind_(index),
      nullable_(nullable),
      unique_(unique) {
  ASSERT(type == TypeId::kTypeChar, "Wrong constructor for non-VARCHAR type.");
}

Column::Column(const Column *other)
    : name_(other->name_),
      type_(other->type_),
      len_(other->len_),
      table_ind_(other->table_ind_),
      nullable_(other->nullable_),
      unique_(other->unique_) {}

/**
* TODO: Student Implement
*/
uint32_t Column::SerializeTo(char *buf) const {
  //依旧拿p做指针开写
  char *p = buf;

  MACH_WRITE_UINT32(p, COLUMN_MAGIC_NUM);
  p += sizeof(uint32_t);

  MACH_WRITE_UINT32(p, name_.length());
  p += sizeof(uint32_t);

  MACH_WRITE_STRING(p, name_);
  p += name_.length();

  MACH_WRITE_TO(TypeId, p, type_);
  p += sizeof(TypeId);

  MACH_WRITE_UINT32(p, len_);
  p += sizeof(uint32_t);

  MACH_WRITE_UINT32(p, table_ind_);
  p += sizeof(uint32_t);

  MACH_WRITE_TO(bool, p, nullable_);
  p += sizeof(bool);

  MACH_WRITE_TO(bool, p, unique_);
  p += sizeof(bool);

  //返回序列化后的总长度
  return p - buf;
}

/**
 * TODO: Student Implement
 */
uint32_t Column::GetSerializedSize() const {
  return sizeof(uint32_t) + sizeof(uint32_t) + name_.length() + sizeof(type_) + sizeof(len_) + sizeof(table_ind_) +
         sizeof(nullable_) + sizeof(unique_);
  //magic + name_len + name + type + len + table_ind + nullable + unique
}

/**
 * TODO: Student Implement
 */
uint32_t Column::DeserializeFrom(char *buf, Column *&column) {
  //按照上面反着来
  uint32_t off = 0;

  uint32_t magic = MACH_READ_UINT32(buf);
  off += sizeof(uint32_t);
  ASSERT(magic == COLUMN_MAGIC_NUM, "Invalid column magic number.");

  //本质就是倒着做hh
  uint32_t name_len = MACH_READ_UINT32(buf + off);
  off += sizeof(uint32_t);
  std::string column_name(buf + off, name_len);
  off += name_len;

  TypeId type = MACH_READ_FROM(TypeId, buf + off);
  off += sizeof(TypeId);

  uint32_t len = MACH_READ_UINT32(buf + off);
  off += sizeof(uint32_t);

  uint32_t table_ind = MACH_READ_UINT32(buf + off);
  off += sizeof(uint32_t);

  bool nullable = MACH_READ_FROM(bool, buf + off);
  off += sizeof(bool);

  bool unique = MACH_READ_FROM(bool, buf + off);
  off += sizeof(bool);

  //需要区分KTypeChar和其他类型
  if(type == TypeId::kTypeChar) {
    column = new Column(column_name, type, len, table_ind, nullable, unique);
  } else {
    column = new Column(column_name, type, table_ind, nullable, unique);
  }
  return off;
}
