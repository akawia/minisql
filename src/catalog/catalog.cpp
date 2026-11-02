#include "catalog/catalog.h"

#include "page/index_roots_page.h"

void CatalogMeta::SerializeTo(char *buf) const {
  ASSERT(GetSerializedSize() <= PAGE_SIZE, "Failed to serialize catalog metadata to disk.");//失败了
  MACH_WRITE_UINT32(buf, CATALOG_METADATA_MAGIC_NUM);//写入魔数//src/include/common/macros.h
  buf += 4;//移动到下一个字段的位置
  MACH_WRITE_UINT32(buf, table_meta_pages_.size());//表数量
  buf += 4;
  MACH_WRITE_UINT32(buf, index_meta_pages_.size());//索引数量
  buf += 4;
  for (auto iter : table_meta_pages_) {
    MACH_WRITE_TO(table_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
  for (auto iter : index_meta_pages_) {
    MACH_WRITE_TO(index_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
}//依次写入所有

CatalogMeta *CatalogMeta::DeserializeFrom(char *buf) {//恢复目录元信息
  // check valid
  uint32_t magic_num = MACH_READ_UINT32(buf);//读取魔数
  buf += 4;//下一字段
  ASSERT(magic_num == CATALOG_METADATA_MAGIC_NUM, "Failed to deserialize catalog metadata from disk.");
  // get table and index nums
  uint32_t table_nums = MACH_READ_UINT32(buf);
  buf += 4;
  uint32_t index_nums = MACH_READ_UINT32(buf);
  buf += 4;
  // create metadata and read value
  CatalogMeta *meta = new CatalogMeta();//构建新的catalogmeta并返回
  for (uint32_t i = 0; i < table_nums; i++) {
    auto table_id = MACH_READ_FROM(table_id_t, buf);
    buf += 4;
    auto table_heap_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->table_meta_pages_.emplace(table_id, table_heap_page_id);
  }
  for (uint32_t i = 0; i < index_nums; i++) {
    auto index_id = MACH_READ_FROM(index_id_t, buf);
    buf += 4;
    auto index_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->index_meta_pages_.emplace(index_id, index_page_id);
  }//依次读取
  return meta;
}

uint32_t CatalogMeta::GetSerializedSize() const {//计算序列化后的字节数
  return 3 * sizeof(uint32_t) + (table_meta_pages_.size() + index_meta_pages_.size()) * 2 * sizeof(uint32_t);
}//3*代表magic num、表数量、索引数量，后面是每个表和索引的id和页id

CatalogMeta::CatalogMeta() {}

CatalogManager::CatalogManager(BufferPoolManager *buffer_pool_manager, LockManager *lock_manager,
                               LogManager *log_manager, bool init)
    : buffer_pool_manager_(buffer_pool_manager), lock_manager_(lock_manager), log_manager_(log_manager) {
  if (init) {
    catalog_meta_ = CatalogMeta::NewInstance();
    next_table_id_ = catalog_meta_->GetNextTableId();
    next_index_id_ = catalog_meta_->GetNextIndexId();//新建数据库
    FlushCatalogMetaPage();
  } else {//打开已有数据库
    auto page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);//读取
    catalog_meta_ = CatalogMeta::DeserializeFrom(page->GetData());//反序列化
    buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID, false);
    next_table_id_ = catalog_meta_->GetNextTableId();//回复表
    next_index_id_ = catalog_meta_->GetNextIndexId();//恢复索引
    for (const auto &entry : catalog_meta_->table_meta_pages_) {
      LoadTable(entry.first, entry.second);
    }
    for (const auto &entry : catalog_meta_->index_meta_pages_) {
      LoadIndex(entry.first, entry.second);
    }
  }
}

CatalogManager::~CatalogManager() {
  FlushCatalogMetaPage();
  delete catalog_meta_;
  for (auto iter : tables_) {
    delete iter.second;
  }
  for (auto iter : indexes_) {
    delete iter.second;
  }
}

dberr_t CatalogManager::CreateTable(const string &table_name, TableSchema *schema, Txn *txn, TableInfo *&table_info) {
  if (table_names_.find(table_name) != table_names_.end()) {
    table_info = nullptr;
    return DB_TABLE_ALREADY_EXIST;
  }//表存在

  table_id_t table_id = next_table_id_++;
  TableSchema *table_schema = Schema::DeepCopySchema(schema);//拷贝内部结构
  TableHeap *table_heap = TableHeap::Create(buffer_pool_manager_, table_schema, txn, log_manager_, lock_manager_);//创建首页
  TableMetadata *table_meta =
      TableMetadata::Create(table_id, table_name, table_heap->GetFirstPageId(), table_schema);
//创建并序列化
  page_id_t meta_page_id;
  auto meta_page = buffer_pool_manager_->NewPage(meta_page_id);
  if (meta_page == nullptr) {
    delete table_meta;
    delete table_heap;
    return DB_FAILED;
  }
  table_meta->SerializeTo(meta_page->GetData());
  buffer_pool_manager_->UnpinPage(meta_page_id, true);

  table_info = TableInfo::Create();
  table_info->Init(table_meta, table_heap);
  table_names_[table_name] = table_id;
  tables_[table_id] = table_info;
  catalog_meta_->table_meta_pages_[table_id] = meta_page_id;//登记
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

dberr_t CatalogManager::GetTable(const string &table_name, TableInfo *&table_info) {//通过 `table_names_` 找到 `table_id`，再到 `tables_` 中取出 `TableInfo`。
  auto iter = table_names_.find(table_name);
  if (iter == table_names_.end()) {
    table_info = nullptr;
    return DB_TABLE_NOT_EXIST;
  }
  return GetTable(iter->second, table_info);
}

dberr_t CatalogManager::GetTables(vector<TableInfo *> &tables) const {//遍历
  tables.clear();
  for (const auto &entry : tables_) {
    tables.emplace_back(entry.second);
  }
  return tables.empty() ? DB_FAILED : DB_SUCCESS;
}

dberr_t CatalogManager::CreateIndex(const std::string &table_name, const string &index_name,
                                    const std::vector<std::string> &index_keys, Txn *txn, IndexInfo *&index_info,
                                    const string &index_type) {
  if (index_type != "bptree") {
    index_info = nullptr;
    return DB_FAILED;
  }
  TableInfo *table_info = nullptr;
  auto table_status = GetTable(table_name, table_info);
  if (table_status != DB_SUCCESS) {
    index_info = nullptr;
    return table_status;
  }
  auto table_index_iter = index_names_.find(table_name);
  if (table_index_iter != index_names_.end() && table_index_iter->second.find(index_name) != table_index_iter->second.end()) {
    index_info = nullptr;
    return DB_INDEX_ALREADY_EXIST;
  }

  std::vector<uint32_t> key_map;//构造k_map
  key_map.reserve(index_keys.size());
  for (const auto &key : index_keys) {
    uint32_t column_index;
    auto column_status = table_info->GetSchema()->GetColumnIndex(key, column_index);
    if (column_status != DB_SUCCESS) {
      index_info = nullptr;
      return column_status;
    }
    key_map.emplace_back(column_index);
  }

  index_id_t index_id = next_index_id_++;//分配新的index_id和索引元信息页
  IndexMetadata *index_meta = IndexMetadata::Create(index_id, index_name, table_info->GetTableId(), key_map);
  page_id_t meta_page_id;
  auto meta_page = buffer_pool_manager_->NewPage(meta_page_id);
  if (meta_page == nullptr) {
    delete index_meta;
    index_info = nullptr;
    return DB_FAILED;
  }
  index_meta->SerializeTo(meta_page->GetData());
  buffer_pool_manager_->UnpinPage(meta_page_id, true);

  index_info = IndexInfo::Create();
  index_info->Init(index_meta, table_info, buffer_pool_manager_);
  for (auto iter = table_info->GetTableHeap()->Begin(txn); iter != table_info->GetTableHeap()->End(); ++iter) {
    Row key_row;
    iter->GetKeyFromRow(table_info->GetSchema(), index_info->GetIndexKeySchema(), key_row);
    index_info->GetIndex()->InsertEntry(key_row, iter->GetRowId(), txn);
  }//遍历表中信息
  index_names_[table_name][index_name] = index_id;
  indexes_[index_id] = index_info;
  catalog_meta_->index_meta_pages_[index_id] = meta_page_id;//更新
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

dberr_t CatalogManager::GetIndex(const std::string &table_name, const std::string &index_name,
                                 IndexInfo *&index_info) const {
  auto table_iter = index_names_.find(table_name);
  if (table_iter == index_names_.end()) {
    index_info = nullptr;
    return DB_INDEX_NOT_FOUND;
  }
  auto index_iter = table_iter->second.find(index_name);
  if (index_iter == table_iter->second.end()) {
    index_info = nullptr;
    return DB_INDEX_NOT_FOUND;
  }
  index_info = indexes_.at(index_iter->second);
  return DB_SUCCESS;
}

dberr_t CatalogManager::GetTableIndexes(const std::string &table_name, std::vector<IndexInfo *> &indexes) const {
  indexes.clear();
  auto table_iter = index_names_.find(table_name);
  if (table_iter == index_names_.end()) {
    return DB_INDEX_NOT_FOUND;
  }
  for (const auto &entry : table_iter->second) {
    indexes.emplace_back(indexes_.at(entry.second));
  }
  return indexes.empty() ? DB_INDEX_NOT_FOUND : DB_SUCCESS;
}

dberr_t CatalogManager::DropTable(const string &table_name) {//删除表
  auto iter = table_names_.find(table_name);//找到
  if (iter == table_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  return DropTable(iter->second);
}

dberr_t CatalogManager::DropIndex(const string &table_name, const string &index_name) {
  auto table_iter = index_names_.find(table_name);
  if (table_iter == index_names_.end()) {
    return DB_INDEX_NOT_FOUND;
  }
  auto index_iter = table_iter->second.find(index_name);
  if (index_iter == table_iter->second.end()) {
    return DB_INDEX_NOT_FOUND;
  }
  index_id_t index_id = index_iter->second;
  auto info_iter = indexes_.find(index_id);
  if (info_iter == indexes_.end()) {
    return DB_INDEX_NOT_FOUND;
  }

  info_iter->second->GetIndex()->Destroy();
  auto roots_page = reinterpret_cast<IndexRootsPage *>(buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID)->GetData());
  roots_page->Delete(index_id);
  buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, true);
  catalog_meta_->DeleteIndexMetaPage(buffer_pool_manager_, index_id);
  delete info_iter->second;
  indexes_.erase(info_iter);
  table_iter->second.erase(index_iter);
  if (table_iter->second.empty()) {
    index_names_.erase(table_iter);
  }
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

dberr_t CatalogManager::FlushCatalogMetaPage() const {
  auto page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
  if (page == nullptr) {
    return DB_FAILED;
  }
  catalog_meta_->SerializeTo(page->GetData());
  buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID, true);
  return DB_SUCCESS;
}

dberr_t CatalogManager::LoadTable(const table_id_t table_id, const page_id_t page_id) {
  auto page = buffer_pool_manager_->FetchPage(page_id);
  if (page == nullptr) {
    return DB_FAILED;
  }
  TableMetadata *table_meta = nullptr;
  TableMetadata::DeserializeFrom(page->GetData(), table_meta);
  buffer_pool_manager_->UnpinPage(page_id, false);

  TableHeap *table_heap =
      TableHeap::Create(buffer_pool_manager_, table_meta->GetFirstPageId(), table_meta->GetSchema(), log_manager_,
                        lock_manager_);
  TableInfo *table_info = TableInfo::Create();
  table_info->Init(table_meta, table_heap);
  table_names_[table_meta->GetTableName()] = table_id;
  tables_[table_id] = table_info;
  return DB_SUCCESS;
}

dberr_t CatalogManager::LoadIndex(const index_id_t index_id, const page_id_t page_id) {
  auto page = buffer_pool_manager_->FetchPage(page_id);
  if (page == nullptr) {
    return DB_FAILED;
  }
  IndexMetadata *index_meta = nullptr;
  IndexMetadata::DeserializeFrom(page->GetData(), index_meta);
  buffer_pool_manager_->UnpinPage(page_id, false);

  TableInfo *table_info = nullptr;
  if (GetTable(index_meta->GetTableId(), table_info) != DB_SUCCESS) {
    delete index_meta;
    return DB_TABLE_NOT_EXIST;
  }
  IndexInfo *index_info = IndexInfo::Create();
  index_info->Init(index_meta, table_info, buffer_pool_manager_);
  index_names_[table_info->GetTableName()][index_meta->GetIndexName()] = index_id;
  indexes_[index_id] = index_info;
  return DB_SUCCESS;
}

dberr_t CatalogManager::GetTable(const table_id_t table_id, TableInfo *&table_info) {
  auto iter = tables_.find(table_id);
  if (iter == tables_.end()) {
    table_info = nullptr;
    return DB_TABLE_NOT_EXIST;
  }
  table_info = iter->second;
  return DB_SUCCESS;
}

dberr_t CatalogManager::DropTable(table_id_t table_id) {
  auto table_iter = tables_.find(table_id);
  if (table_iter == tables_.end()) {
    return DB_TABLE_NOT_EXIST;
  }

  TableInfo *table_info = table_iter->second;
  std::string table_name = table_info->GetTableName();
  std::vector<std::string> index_names;
  auto index_iter = index_names_.find(table_name);
  if (index_iter != index_names_.end()) {
    for (const auto &entry : index_iter->second) {
      index_names.emplace_back(entry.first);
    }
  }
  for (const auto &index_name : index_names) {
    DropIndex(table_name, index_name);
  }

  table_info->GetTableHeap()->DeleteTable();
  auto meta_iter = catalog_meta_->table_meta_pages_.find(table_id);
  if (meta_iter != catalog_meta_->table_meta_pages_.end()) {
    buffer_pool_manager_->DeletePage(meta_iter->second);
    catalog_meta_->table_meta_pages_.erase(meta_iter);
  }
  table_names_.erase(table_name);
  delete table_info;
  tables_.erase(table_iter);
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}
