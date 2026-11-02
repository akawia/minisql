#include "index/index_iterator.h"

#include "index/basic_comparator.h"
#include "index/generic_key.h"

IndexIterator::IndexIterator() = default;

IndexIterator::IndexIterator(page_id_t page_id, BufferPoolManager *bpm, int index)
    : current_page_id(page_id), item_index(index), buffer_pool_manager(bpm) {
  page = reinterpret_cast<LeafPage *>(buffer_pool_manager->FetchPage(current_page_id)->GetData());
}

IndexIterator::~IndexIterator() {
  if (current_page_id != INVALID_PAGE_ID)
    buffer_pool_manager->UnpinPage(current_page_id, false);
}

/**
 * TODO: Student Implement
 */
std::pair<GenericKey *, RowId> IndexIterator::operator*() {
  //返回当前叶页第 item_index 个键值对
  return page->GetItem(item_index);
}

/**
 * TODO: Student Implement
 */
IndexIterator &IndexIterator::operator++() {
  //本页还有下一个,直接后移一格
  if (item_index + 1 < page->GetSize()) {
    item_index++;
    return *this;
  }

  //本页到头了,顺着叶链表 next 跳到下一个叶页
  page_id_t next_id = page->GetNextPageId();
  buffer_pool_manager->UnpinPage(current_page_id, false);  //离开当前页,unpin

  if (next_id == INVALID_PAGE_ID) {
    //没有下一页 → 变成 end 迭代器
    current_page_id = INVALID_PAGE_ID;
    page = nullptr;
    item_index = 0;
    return *this;
  }

  //换到下一页的第一个键值对(新页被 pin,析构时再 unpin)
  current_page_id = next_id;
  page = reinterpret_cast<LeafPage *>(buffer_pool_manager->FetchPage(next_id)->GetData());
  item_index = 0;
  return *this;
}

bool IndexIterator::operator==(const IndexIterator &itr) const {
  return current_page_id == itr.current_page_id && item_index == itr.item_index;
}

bool IndexIterator::operator!=(const IndexIterator &itr) const {
  return !(*this == itr);
}