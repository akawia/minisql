#include "page/bitmap_page.h"

#include "glog/logging.h"

/**
 * TODO: 
  //分配一个空闲页，并通过page_offset返回所分配的空闲页位于该段中的下标（从0开始）；
 */
template <size_t PageSize>
bool BitmapPage<PageSize>::AllocatePage(uint32_t &page_offset) {
  //if不能分配
  if(page_allocated_ >= GetMaxSupportedSize()) {
    return false;
  }

  page_offset = next_free_page_;
  uint32_t byte_index = page_offset / 8;
  uint8_t bit_index = page_offset % 8;
  //将该页标记为已分配
  bytes[byte_index] |= (1 << bit_index);
  page_allocated_++;

  //更新下一个空闲页的下标
  for (uint32_t i = page_offset + 1; i < GetMaxSupportedSize(); i++) {
    byte_index = i / 8;
    bit_index = i % 8;
    if (IsPageFreeLow(byte_index, bit_index)) {
      next_free_page_ = i;
      return true;
    }
  }
  next_free_page_ = GetMaxSupportedSize();
  return true;
}

/**
 * TODO: 回收已经被分配的页；
 */
template <size_t PageSize>
bool BitmapPage<PageSize>::DeAllocatePage(uint32_t page_offset) {
  //如果超出范围或者本身空闲
  if(page_offset >= GetMaxSupportedSize() || IsPageFree(page_offset)) {
    return false;
  }

  uint32_t byte_index = page_offset / 8;
  uint8_t bit_index = page_offset % 8;

  //对应的位清零，表示该页空闲
  bytes[byte_index] &= ~(1 << bit_index);
  page_allocated_--;
  next_free_page_ = page_offset;
  return true;
}

/**
 * TODO: 判断给定的页是否是空闲（未分配）的
 */
template <size_t PageSize>
bool BitmapPage<PageSize>::IsPageFree(uint32_t page_offset) const {
  if(page_offset >= GetMaxSupportedSize()) {
    return false;
  }

  uint32_t byte_index = page_offset / 8;
  uint8_t bit_index = page_offset % 8;

  //判断该页是否空闲
  return IsPageFreeLow(byte_index, bit_index);
}

template <size_t PageSize>
bool BitmapPage<PageSize>::IsPageFreeLow(uint32_t byte_index, uint8_t bit_index) const {
  //返回bit_index位是否为0
  return ((bytes[byte_index] & (1 << bit_index)) == 0);
}

template class BitmapPage<64>;

template class BitmapPage<128>;

template class BitmapPage<256>;

template class BitmapPage<512>;

template class BitmapPage<1024>;

template class BitmapPage<2048>;

template class BitmapPage<4096>;