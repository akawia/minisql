#include "buffer/lru_replacer.h"

LRUReplacer::LRUReplacer(size_t num_pages){}

LRUReplacer::~LRUReplacer() = default;

//我们定义了一个list和一个表来存储lru相关数据
//gogogogo

/**
 * TODO: ：替换与所有被跟踪的页相比最近最少被访问的页，将其页帧号（即数据页在Buffer Pool的Page数组中的下标）
 * 存储在输出参数frame_id中输出并返回true，如果当前没有可以替换的元素则返回false
 */
bool LRUReplacer::Victim(frame_id_t *frame_id) {
  //上一个mutex锁，和其它操作保持一致
  std::lock_guard<std::mutex> lock(latch_);

  //这里我们要删除最旧的
  if(lru_list_.empty()){
    return false;
  }

  //获取最旧的页帧号
  *frame_id = lru_list_.back();

  //从list和set中删除该页
  lru_list_.pop_back();
  lru_set_.erase(*frame_id);

  return true;
}

/**
 * TODO: 将数据页固定使之不能被Replacer替换，即从lru_list_中移除该数据页对应的页帧。
 * Pin函数应当在一个数据页被Buffer Pool Manager固定时被调用
 */
void LRUReplacer::Pin(frame_id_t frame_id) {
  //上一个mutex锁
  std::lock_guard<std::mutex> lock(latch_);

  //如果pin在set中找到了该页帧号，则从list和set中删除
  if(lru_set_.find(frame_id) != lru_set_.end()){
    //如果在set中找到了该页帧号，则从list和set中删除
    lru_list_.remove(frame_id);
    lru_set_.erase(frame_id);
  } else {
    //如果不在就反回
    return;
  }
}

/**
 * TODO: 将数据页解除固定，放入lru_list_中，使之可以在必要时被Replacer替换掉。
 * Unpin函数应当在一个数据页的引用计数变为0时被Buffer Pool Manager调用，使页帧对应的数据页能够在必要时被替换
 */
void LRUReplacer::Unpin(frame_id_t frame_id) {
  //上一个mutex锁
  std::lock_guard<std::mutex> lock(latch_);

  //如果在set中找到了该页帧号，则不做任何操作
  if(lru_set_.find(frame_id) != lru_set_.end()){
    return;
  }

  //如果不在set中，则将其加入到list和set中
  lru_list_.push_front(frame_id);
  lru_set_.insert(frame_id);
}

/**
 * TODO: 此方法返回当前LRUReplacer中能够被替换的数据页的数量
 */
size_t LRUReplacer::Size() {
  std::lock_guard<std::mutex> lock(latch_);
  return lru_list_.size();
}