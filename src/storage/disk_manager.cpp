#include "storage/disk_manager.h"

#include <sys/stat.h>

#include <filesystem>
#include <stdexcept>

#include "glog/logging.h"
#include "page/bitmap_page.h"

DiskManager::DiskManager(const std::string &db_file) : file_name_(db_file) {
  //lock上我们的mutex，防止多线程同时访问文件
  std::scoped_lock<std::recursive_mutex> lock(db_io_latch_);
  db_io_.open(db_file, std::ios::binary | std::ios::in | std::ios::out);
  // directory or file does not exist
  if (!db_io_.is_open()) {
    db_io_.clear();
    // create a new file
    std::filesystem::path p = db_file;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    db_io_.open(db_file, std::ios::binary | std::ios::trunc | std::ios::out);
    db_io_.close();
    // reopen with original mode
    db_io_.open(db_file, std::ios::binary | std::ios::in | std::ios::out);
    if (!db_io_.is_open()) {
      throw std::exception();
    }
  }
  ReadPhysicalPage(META_PAGE_ID, meta_data_);
}

void DiskManager::Close() {
  std::scoped_lock<std::recursive_mutex> lock(db_io_latch_);
  WritePhysicalPage(META_PAGE_ID, meta_data_);
  if (!closed) {
    db_io_.close();
    closed = true;
  }
}

void DiskManager::ReadPage(page_id_t logical_page_id, char *page_data) {
  ASSERT(logical_page_id >= 0, "Invalid page id.");
  ReadPhysicalPage(MapPageId(logical_page_id), page_data);
}

void DiskManager::WritePage(page_id_t logical_page_id, const char *page_data) {
  ASSERT(logical_page_id >= 0, "Invalid page id.");
  WritePhysicalPage(MapPageId(logical_page_id), page_data);
}

/**
 * TODO: 从磁盘中分配一个空闲页，并返回空闲页的逻辑页号
 */
page_id_t DiskManager::AllocatePage() {
  //meta
  auto *meta = reinterpret_cast<DiskFileMetaPage *>(meta_data_);

  //寻找空闲页
  uint32_t extend_id = 0;
  while(extend_id < meta -> num_extents_ && meta -> extent_used_page_[extend_id] == BITMAP_SIZE) {
    extend_id++;
  }

  //如果meta内满了
  if(extend_id == meta -> num_extents_){
    meta -> num_extents_++;
  } 

  page_id_t bitmap_phys = extend_id * (BITMAP_SIZE + 1) + 1;
  char buf[PAGE_SIZE];
  //读取这一页的bitmap然后分配一个页
  ReadPhysicalPage(bitmap_phys, buf);
  auto *bitmap_page = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(buf);

  uint32_t page_offset;
  //bitmap类里面实现的
  bitmap_page -> AllocatePage(page_offset);
  WritePhysicalPage(bitmap_phys, buf);

  //更新meta
  meta -> num_allocated_pages_++;
  meta -> extent_used_page_[extend_id]++;

  return extend_id * BITMAP_SIZE + page_offset;
}

/**
 * TODO: 释放磁盘中逻辑页号对应的物理页。
 */
void DiskManager::DeAllocatePage(page_id_t logical_page_id) {
  auto *meta = reinterpret_cast<DiskFileMetaPage *>(meta_data_);

  uint32_t extend_id = logical_page_id / BITMAP_SIZE;
  uint32_t page_offset = logical_page_id % BITMAP_SIZE;

  //计算出对应的bitmap页的物理页号
  page_id_t bitmap_phys = extend_id * (BITMAP_SIZE + 1) + 1;
  char buf[PAGE_SIZE];
  ReadPhysicalPage(bitmap_phys, buf);
  auto *bitmap_page = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(buf);

  //回收该页
  bitmap_page -> DeAllocatePage(page_offset);
  WritePhysicalPage(bitmap_phys, buf);

  //修改元数据  
  meta -> extent_used_page_[extend_id]--;
  meta -> num_allocated_pages_--;
}

/**
 * TODO: 判断该逻辑页号对应的数据页是否空闲
 */
bool DiskManager::IsPageFree(page_id_t logical_page_id) {
  //extend_id和page_offset
  uint32_t extend_id = logical_page_id / BITMAP_SIZE;
  uint32_t page_offset = logical_page_id % BITMAP_SIZE;

  //计算出对应的bitmap页的物理页号
  page_id_t bitmap_phys = extend_id * (BITMAP_SIZE + 1) + 1;
  char buf[PAGE_SIZE];
  ReadPhysicalPage(bitmap_phys, buf);
  auto *bitmap_page = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(buf);

  //Ispagefree?
  return bitmap_page -> IsPageFree(page_offset);
}

/**
 * TODO:可根据需要实现。在DiskManager类的私有成员中，该函数可以用于将逻辑页号转换成物理页号。
 */
page_id_t DiskManager::MapPageId(page_id_t logical_page_id) {
  //extend_id和page_offset
  uint32_t extend_id = logical_page_id / BITMAP_SIZE;
  uint32_t page_offset = logical_page_id % BITMAP_SIZE;

  //计算出对应的bitmap页的物理页号
  page_id_t bitmap_phys = extend_id * (BITMAP_SIZE + 1) + 1;

  return bitmap_phys + page_offset + 1;
}

int DiskManager::GetFileSize(const std::string &file_name) {
  struct stat stat_buf;
  int rc = stat(file_name.c_str(), &stat_buf);
  return rc == 0 ? stat_buf.st_size : -1;
}

void DiskManager::ReadPhysicalPage(page_id_t physical_page_id, char *page_data) {
  int offset = physical_page_id * PAGE_SIZE;
  // check if read beyond file length
  if (offset >= GetFileSize(file_name_)) {
#ifdef ENABLE_BPM_DEBUG
    LOG(INFO) << "Read less than a page" << std::endl;
#endif
    memset(page_data, 0, PAGE_SIZE);
  } else {
    // set read cursor to offset
    db_io_.seekp(offset);
    db_io_.read(page_data, PAGE_SIZE);
    // if file ends before reading PAGE_SIZE
    int read_count = db_io_.gcount();
    if (read_count < PAGE_SIZE) {
#ifdef ENABLE_BPM_DEBUG
      LOG(INFO) << "Read less than a page" << std::endl;
#endif
      memset(page_data + read_count, 0, PAGE_SIZE - read_count);
    }
  }
}

void DiskManager::WritePhysicalPage(page_id_t physical_page_id, const char *page_data) {
  size_t offset = static_cast<size_t>(physical_page_id) * PAGE_SIZE;
  // set write cursor to offset
  db_io_.seekp(offset);
  db_io_.write(page_data, PAGE_SIZE);
  // check for I/O error
  if (db_io_.bad()) {
    LOG(ERROR) << "I/O error while writing";
    return;
  }
  // needs to flush to keep disk file in sync
  db_io_.flush();
}