#include "storage/table_iterator.h"

#include "common/macros.h"
#include "storage/table_heap.h"

/**
 * 记下所属堆表和 rid;若 rid 有效,顺便把记录内容读出来缓存到 row_
 */
TableIterator::TableIterator(TableHeap *table_heap, RowId rid, Txn *txn)
    : table_heap_(table_heap), txn_(txn) {
  //拿出row然后根据rid去tableheap里找记录内容
  row_ = new Row(rid);
  if (rid.GetPageId() != INVALID_PAGE_ID) {
    table_heap_->GetTuple(row_, txn_);
  }
}

TableIterator::TableIterator(const TableIterator &other)
    : table_heap_(other.table_heap_), txn_(other.txn_) {
  row_ = new Row(*other.row_);
}

TableIterator::~TableIterator() { delete row_; }

bool TableIterator::operator==(const TableIterator &itr) const {
  //迭代器相等的条件是所属堆表相同,且rid相同
  return table_heap_ == itr.table_heap_ && row_->GetRowId() == itr.row_->GetRowId();
}

bool TableIterator::operator!=(const TableIterator &itr) const {
  return !(*this == itr); 
}

const Row &TableIterator::operator*() {
  ASSERT(row_->GetRowId().GetPageId() != INVALID_PAGE_ID, "Dereference end iterator.");
  return *row_;
}

Row *TableIterator::operator->() {
  ASSERT(row_->GetRowId().GetPageId() != INVALID_PAGE_ID, "Dereference end iterator.");
  return row_;
}

TableIterator &TableIterator::operator=(const TableIterator &itr) noexcept {
  //赋值
  if (this != &itr) {
    //先释放原来的row_,再拷贝新的
    table_heap_ = itr.table_heap_;
    txn_ = itr.txn_;
    delete row_;
    row_ = new Row(*itr.row_);
  }
  return *this;
}

//++iter
TableIterator &TableIterator::operator++() {
  //迭代器在bufferpool里找
  BufferPoolManager *bpm = table_heap_->buffer_pool_manager_;

  //拿出当前page
  auto cur_page = reinterpret_cast<TablePage *>(bpm->FetchPage(row_->GetRowId().GetPageId()));
  cur_page->RLatch();

  RowId next_rid;
  bool found = cur_page->GetNextTupleRid(row_->GetRowId(), &next_rid);

  //如果当前页没有下一条记录,就去下一页找第一条记录
  while (!found && cur_page->GetNextPageId() != INVALID_PAGE_ID) {
    auto next_page_id = cur_page->GetNextPageId();
    cur_page->RUnlatch();
    bpm->UnpinPage(cur_page->GetTablePageId(), false);

    cur_page = reinterpret_cast<TablePage *>(bpm->FetchPage(next_page_id));
    cur_page->RLatch();

    found = cur_page->GetFirstTupleRid(&next_rid);
  }

  //如果找到了下一条记录,就更新row_为下一条记录;否则就变成尾迭代器
  cur_page->RUnlatch();
  bpm->UnpinPage(cur_page->GetTablePageId(), false);

  delete row_;
  if (found) {
    row_ = new Row(next_rid);
    table_heap_->GetTuple(row_, txn_);
  } else {
    row_ = new Row(INVALID_ROWID);
  }
  return *this;
}

// iter++:先存副本,再自增,返回旧值
TableIterator TableIterator::operator++(int) {
  TableIterator temp(*this);
  ++(*this);
  return temp;
}
