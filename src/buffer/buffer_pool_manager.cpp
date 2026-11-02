#include "buffer/buffer_pool_manager.h"

#include "glog/logging.h"
#include "page/bitmap_page.h"

static const char EMPTY_PAGE_DATA[PAGE_SIZE] = {0};

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager) {
  pages_ = new Page[pool_size_];
  replacer_ = new LRUReplacer(pool_size_);
  for (size_t i = 0; i < pool_size_; i++) {
    free_list_.emplace_back(i);
  }
}

BufferPoolManager::~BufferPoolManager() {
  for (auto page : page_table_) {
    FlushPage(page.first);
  }
  delete[] pages_;
  delete replacer_;
}

/**
 * 找一个可用的 frame：优先从空闲列表取，否则让 replacer 淘汰一个。
 * 只负责"挑出一个 frame"，不做脏页写回/页表清理——这些由调用方负责。
 * @return 可用的 frame_id；若无空闲页也无可淘汰页，返回 -1。
 */
frame_id_t BufferPoolManager::TryToFindFreePage() {
  frame_id_t frame_id;
  
  //优先使用空闲列表里的 frame
  if (!free_list_.empty()) {
    frame_id = free_list_.front();
    free_list_.pop_front();
    return frame_id;
  }
  //如果没有 那就让 replacer 淘汰一个
  if (!replacer_->Victim(&frame_id)) {
    return -1;
  }
  return frame_id;
}

/**
 * 根据逻辑页号获取对应的数据页，如果该数据页不在内存中，则需要从磁盘中进行读取；
 */
Page *BufferPoolManager::FetchPage(page_id_t page_id) {
  std::lock_guard<std::recursive_mutex> lock(latch_);

  // 1.     Search the page table for the requested page (P).
  // 1.1    If P exists, pin it and return it immediately.
  auto it = page_table_.find(page_id);
  if (it != page_table_.end()) {
    frame_id_t frame_id = it->second;

    Page *page = &pages_[frame_id];
    page->pin_count_++;
    //pin住然后返回
    replacer_->Pin(frame_id);
    return page;
  }

  // 1.2    If P does not exist, find a replacement page (R) from either the free list or the replacer.
  //Note that pages are always found from the free list first.
  frame_id_t frame_id = TryToFindFreePage();
  if (frame_id == -1) {
    return nullptr;
  }

  // 2.     If R is dirty, write it back to the disk.
  Page *page = &pages_[frame_id];
  if (page->IsDirty()) {
    disk_manager_->WritePage(page->GetPageId(), page->GetData());
    page->is_dirty_ = false;
  }

  // 3.     Delete R from the page table and insert P.
  page_table_.erase(page->GetPageId());
  page_table_[page_id] = frame_id;

  // 4.     Update P's metadata, read in the page content from disk, and then return a pointer to P.
  page->page_id_ = page_id;
  page->pin_count_ = 1;

  //读取数据页到内存 pin住
  disk_manager_->ReadPage(page_id, page->GetData());
  replacer_->Pin(frame_id);
  return page;
}

/**
 * 分配一个新的数据页，并将逻辑页号于page_id中返回；
 */
Page *BufferPoolManager::NewPage(page_id_t &page_id) {
  std::lock_guard<std::recursive_mutex> lock(latch_);
  // 0.   Make sure you call AllocatePage!
  // 1.   If all the pages in the buffer pool are pinned, return nullptr.
  frame_id_t frame_id = TryToFindFreePage();

  if (frame_id == -1) {
    return nullptr;
  }
  page_id = AllocatePage();

  // 2.   Pick a victim page P from either the free list or the replacer. Always pick from the free list first.
  //we done it in TryToFindFreePage()

  // 3.   Update P's metadata, zero out memory and add P to the page table.
  Page *page = &pages_[frame_id];

  if (page->IsDirty()) {
    disk_manager_->WritePage(page->GetPageId(), page->GetData());
    page->is_dirty_ = false;
  }
  page_table_.erase(page->GetPageId());

  //修改页元数据然后读取数据页到内存 pin住
  page->page_id_ = page_id;
  page->pin_count_ = 1;
  page->is_dirty_ = false;
  page->ResetMemory();

  page_table_[page_id] = frame_id;
  replacer_->Pin(frame_id);
  return page;
}

/**
 * 释放一个数据页。
 */
bool BufferPoolManager::DeletePage(page_id_t page_id) {
  std::lock_guard<std::recursive_mutex> lock(latch_);

  // 1.   If P does not exist, return true.
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return true;
  }


  // 2.   If P exists, but has a non-zero pin-count, return false. Someone is using the page.
  frame_id_t frame_id = it->second;
  Page *page = &pages_[frame_id];
  if (page->pin_count_ != 0) {
    return false;
  }

  // 3.   Otherwise, P can be deleted. Remove P from the page table, reset its metadata and return it to the free list.
  DeallocatePage(page_id);
  page_table_.erase(it);

  replacer_->Pin(frame_id);

  //重置页元数据
  page->page_id_ = INVALID_PAGE_ID;
  page->pin_count_ = 0;
  page->is_dirty_ = false;
  page->ResetMemory();
  free_list_.push_back(frame_id);
  return true;
}

/**
 * 取消固定一个数据页
 */
bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  std::lock_guard<std::recursive_mutex> lock(latch_);

  //同之前的还是先找页表确认页在不在缓冲池里
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;
  }

  frame_id_t frame_id = it->second;
  Page *page = &pages_[frame_id];

  //判断dirty位，更新页状态
  if (is_dirty) {
    page->is_dirty_ = true;
  }

  //如果pin小于0非法
  if (page->pin_count_ <= 0) {
    return false;
  }

  page->pin_count_--;
  if (page->pin_count_ == 0) {
    //pin变成0就可以被替换了
    replacer_->Unpin(frame_id);
  }
  return true;
}

/**
 * 将数据页转储到磁盘中
 */
bool BufferPoolManager::FlushPage(page_id_t page_id) {
  std::lock_guard<std::recursive_mutex> lock(latch_);

  //还是先要找到这个page
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;
  }

  frame_id_t frame_id = it->second;
  Page *page = &pages_[frame_id];

  //找到了这个页，就写回磁盘
  disk_manager_->WritePage(page_id, page->GetData());
  page->is_dirty_ = false;
  return true;
}

/**
 * 将所有的页面都转储到磁盘中
 */
void BufferPoolManager::FlushAllPages() {
  std::lock_guard<std::recursive_mutex> lock(latch_);
  //遍历页表里的每个页，写回磁盘
  for (auto &entry : page_table_) {
    FlushPage(entry.first);
  }
}

page_id_t BufferPoolManager::AllocatePage() {
  int next_page_id = disk_manager_->AllocatePage();
  return next_page_id;
}

void BufferPoolManager::DeallocatePage(__attribute__((unused)) page_id_t page_id) {
  disk_manager_->DeAllocatePage(page_id);
}

bool BufferPoolManager::IsPageFree(page_id_t page_id) {
  return disk_manager_->IsPageFree(page_id);
}

// Only used for debug
bool BufferPoolManager::CheckAllUnpinned() {
  bool res = true;
  for (size_t i = 0; i < pool_size_; i++) {
    if (pages_[i].pin_count_ != 0) {
      res = false;
      LOG(ERROR) << "page " << pages_[i].page_id_ << " pin count:" << pages_[i].pin_count_ << endl;
    }
  }
  return res;
}