#include "storage/table_heap.h"

/**
 * TODO: 向堆表中插入一条记录，插入记录后生成的RowId需要通过row对象返回（即row.rid_）；
 */
bool TableHeap::InsertTuple(Row &row, Txn *txn) { 
  //我们现fetchpage 来插入，插入失败就继续fetch下一页，直到成功或者没有页了
  auto page_id = first_page_id_;
  while (page_id != INVALID_PAGE_ID) {
    //拿出页来插入
    auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(page_id));
    assert(page != nullptr);
    page->WLatch();
    //插入成功的话
    if (page->InsertTuple(row, schema_, txn, lock_manager_, log_manager_)) {
      page->WUnlatch();
      buffer_pool_manager_->UnpinPage(page_id, true);
      return true;
    }
    page->WUnlatch();

    //拿出下一页
    auto next_page_id = page->GetNextPageId();
    buffer_pool_manager_->UnpinPage(page_id, false);
    page_id = next_page_id;
  }

  //如果没有页了，就新建一页来插入
  page_id_t new_page_id;
  auto new_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->NewPage(new_page_id));
  assert(new_page != nullptr);
  
  //初始化newpage
  new_page->Init(new_page_id, INVALID_PAGE_ID, log_manager_, txn);
  new_page->WLatch();
  bool insert_success = new_page->InsertTuple(row, schema_, txn, lock_manager_, log_manager_);
  assert(insert_success);
  new_page->WUnlatch();
  buffer_pool_manager_->UnpinPage(new_page_id, true);

  return true;
}

bool TableHeap::MarkDelete(const RowId &rid, Txn *txn) {
  // Find the page which contains the tuple.
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  // If the page could not be found, then abort the recovery.
  if (page == nullptr) {
    return false;
  }
  // Otherwise, mark the tuple as deleted.
  page->WLatch();
  page->MarkDelete(rid, txn, lock_manager_, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
  return true;
}

/**
 * TODO: 将RowId为rid的记录old_row替换成新的记录new_row，并将new_row的RowId通过new_row.rid_返回
 */
bool TableHeap::UpdateTuple(Row &row, const RowId &rid, Txn *txn) {
  //x先fetch一个旧页尝试更新
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));

  if(page == nullptr) {
    return false;
  }
  
  Row old_row(rid);
  page->WLatch();
  //更新成功的话
  bool update_success = page->UpdateTuple(row, &old_row, schema_, txn, lock_manager_, log_manager_);
  page->WUnlatch();

  if(update_success) {
    //更新成功的话 同一个slot
    row.SetRowId(rid);
    buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
    return true;
  }

  //如果空间不够?
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), false);

  //先删除旧记录
  MarkDelete(rid, txn);
  ApplyDelete(rid, txn);
  //再插入新记录
  return InsertTuple(row, txn);
}

/**
 * TODO: 从物理意义上删除这条记录；
 */
void TableHeap::ApplyDelete(const RowId &rid, Txn *txn) {
  // Step1: Find the page which contains the tuple.
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  assert(page != nullptr);

  if(page->GetTablePageId() != rid.GetPageId()) {
    buffer_pool_manager_->UnpinPage(page->GetTablePageId(), false);
    return;
  }

  // Step2: Delete the tuple from the page.
  page->WLatch();
  page->ApplyDelete(rid, txn, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
  
}

void TableHeap::RollbackDelete(const RowId &rid, Txn *txn) {
  // Find the page which contains the tuple.
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  assert(page != nullptr);
  // Rollback to delete.
  page->WLatch();
  page->RollbackDelete(rid, txn, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
}

/**
 * TODO:获取RowId为row->rid_的记录
 */
bool TableHeap::GetTuple(Row *row, Txn *txn) {
  //依旧先fetch页
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(row->GetRowId().GetPageId()));
  if(page == nullptr) {
    return false;
  }

  //根据rowid拿到记录
  page->RLatch();
  bool get_success = page->GetTuple(row, schema_, txn, lock_manager_);
  page->RUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), false);
  return get_success;
}

void TableHeap::DeleteTable(page_id_t page_id) {
  //如果指定了page_id，就从这个页开始删除table_heap
  if (page_id != INVALID_PAGE_ID) {
    //删除指定页及其后续页
    auto temp_table_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(page_id));
    if (temp_table_page->GetNextPageId() != INVALID_PAGE_ID)
      DeleteTable(temp_table_page->GetNextPageId());
    
    buffer_pool_manager_->UnpinPage(page_id, false);
    buffer_pool_manager_->DeletePage(page_id);
  } else {
    DeleteTable(first_page_id_);
  }
}

/**
 * TODO: ：获取堆表的首迭代器；
 */
TableIterator TableHeap::Begin(Txn *txn) {
  //依旧遍历页来找第一个记录
  auto page_id = first_page_id_;
  RowId first_row_id;

  while(page_id != INVALID_PAGE_ID) {
    //拿出页来找第一个记录
    auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(page_id));
    page->RLatch();
    bool found = page->GetFirstTupleRid(&first_row_id);
    page->RUnlatch();

    auto next_page_id = page->GetNextPageId();
    buffer_pool_manager_->UnpinPage(page_id, false);

    if(found) {
      return TableIterator(this, first_row_id, txn);
    }
    page_id = next_page_id;
  }
  //如果没有记录了，就返回end迭代器
  return TableIterator(this, RowId(), txn);
}

/**
 * TODO: 获取堆表的尾迭代器；
 */
TableIterator TableHeap::End() {
  //直接返回end迭代器
  return TableIterator(this, INVALID_ROWID, nullptr);
}
