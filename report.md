# MiniSQL 实验报告

# 1. DISK AND BUFFER POOL MANAGER

本章实现 MiniSQL 架构最底层的两个模块：负责数据页分配，回收与读写的 **Disk Manager**，以及负责在内存与磁盘间调度数据页的 **Buffer Pool Manager**。前者通过**Bitmap**管理页的分配状态，并以"分区（Extent）+ 逻辑/物理页号映射"扩展单文件容量；后者借助 LRU 替换策略，使数据库能处理超过物理内存大小的数据。
## 1.1 BitmapPage

### 1. 设计

`BitmapPage` 是一个正好占 `PageSize` 的结构，用来记录一个分区内每个数据页的分配情况。其成员包括：

- `bytes[MAX_CHARS]`：位图本体，每 1 个 bit 管理 1 个数据页。
- `page_allocated_`：当前已分配的页数。
- `next_free_page_`：下一个待分配空闲页的下标。

对于分区内下标为 `page_offset` 的页，其在位图中的位置为：

```
byte_index = page_offset / 8
bit_index  = page_offset % 8
```

### 2. 各函数实现说明

**`AllocatePage(&page_offset)`**：分配一个空闲页。
- 若 `page_allocated_` 已达到上限 `GetMaxSupportedSize()`，说明分区已满，返回 `false`；
- 否则把 `next_free_page_` 作为本次分配的页，通过 `bytes[byte_index] |= (1 << bit_index)` 将对应 bit 置 1（标记已分配），`page_allocated_++`；
- 再从当前位置向后扫描，找到下一个空闲 bit 更新 `next_free_page_`；
```cpp
//更新下一个空闲页的下标
for (uint32_t i = page_offset + 1; i < GetMaxSupportedSize(); i++) {
	byte_index = i / 8;
	bit_index = i % 8;
	if (IsPageFreeLow(byte_index, bit_index)) {
		next_free_page_ = i;
		return true;
	}
}
```

**`DeAllocatePage(page_offset)`**：回收一个已分配的页。
- 若页号越界或该页本身就是空闲，返回 `false`；
- 否则通过 `bytes[byte_index] &= ~(1 << bit_index)` 将对应 bit 清 0（标记空闲），`page_allocated_--`；
```cpp
//对应的位清零，表示该页空闲
bytes[byte_index] &= ~(1 << bit_index);
page_allocated_--;
next_free_page_ = page_offset;
```

**`IsPageFree(page_offset)`**：判断给定页是否空闲。
- 越界时返回 `false`；
- 否则调用 `IsPageFreeLow` 判断对应 bit 是否为 0。

**`IsPageFreeLow(byte_index, bit_index)`**：底层位判断。
- 通过 `(bytes[byte_index] & (1 << bit_index)) == 0` 判断指定 bit 是否为 0，为 0 即空闲。

## 1.2 磁盘管理 DiskManager

### 1. 分区与逻辑/物理页号映射

单个位图页能管理的页数有限，会造成单文件容量受限。解决思路是把"1 个位图页 + 一段连续数据页"看作一个**分区（Extent）**，再用第 0 页的 **Disk Meta Page** 记录所有分区的信息，从而管理更多数据页。

由于元数据页不存储用户数据，真正存数据的页在物理上是**不连续**的。为了让上层 Buffer Pool Manager 看到连续的页号，DiskManager 维护"逻辑页号 → 物理页号"的映射：逻辑页号只对真正存数据的页从 0 起连续编号。

对任意逻辑页号 `logical_page_id`，先做两步换算：

```
extent_id   = logical_page_id / BITMAP_SIZE   // 属于第几个分区
page_offset = logical_page_id % BITMAP_SIZE   // 在该分区内的偏移
```

### 2. 各函数实现说明

**`MapPageId(logical_page_id)`**：逻辑页号转物理页号。
- 在 `bitmap_phys` 的基础上，再跳过该位图页（`+1`）并加上分区内偏移，得到物理页号：
  `bitmap_phys + page_offset + 1。

**`AllocatePage()`**：分配一个空闲页，返回逻辑页号。
- 取出 `meta`，找到第一个**未满**的分区；若所有分区都满或一个分区都没有，则新建一个分区并 `num_extents_++`；
```cpp
auto *meta = reinterpret_cast<DiskFileMetaPage *>(meta_data_);
//从分区 0 起扫描,跳过所有已满的分区
uint32_t extend_id = 0;
while (extend_id < meta->num_extents_ && meta->extent_used_page_[extend_id] == BITMAP_SIZE) {
	extend_id++;
}
//全满(或一个分区都没有)→ 新建一个分区
if (extend_id == meta->num_extents_) {
	meta->num_extents_++;
}
```
- 读出该分区的位图页，`reinterpret_cast` 成 `BitmapPage<PAGE_SIZE>`，调用其 `AllocatePage` 取得分区内偏移 `page_offset`，再 `WritePhysicalPage` 写回磁盘；
```cpp
//定位该分区的位图页,读改写回:把位操作委托给 BitmapPage
page_id_t bitmap_phys = extend_id * (BITMAP_SIZE + 1) + 1;
char buf[PAGE_SIZE];
ReadPhysicalPage(bitmap_phys, buf);
auto *bitmap_page = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(buf);
uint32_t page_offset;
bitmap_page->AllocatePage(page_offset);
WritePhysicalPage(bitmap_phys, buf);
```
- 更新元数据，返回逻辑页号 `extent_id * BITMAP_SIZE + page_offset`：
```cpp
meta->num_allocated_pages_++;
meta->extent_used_page_[extend_id]++;
return extend_id * BITMAP_SIZE + page_offset;
```

**`DeAllocatePage(logical_page_id)`**：回收逻辑页号对应的物理页。
- 换算出 `extent_id` 与 `page_offset`，读出对应位图页；
- 调用 `BitmapPage::DeAllocatePage(page_offset)` 清除对应 bit，并 `WritePhysicalPage` 写回；
- 更新元数据：`extent_used_page_[extent_id]--`、`num_allocated_pages_--`。

**`IsPageFree(logical_page_id)`**：判断逻辑页是否空闲。
- 换算出 `extent_id` 与 `page_offset`，读出对应位图页，返回 `BitmapPage::IsPageFree(page_offset)`。

## 1.3 LRU

### 1. 设计

`LRUReplacer` 实现了 LRU替换策略，负责跟踪 Buffer Pool 中可被替换的页帧，并在缓冲池没有空闲页时决定淘汰哪一个。

为了让"按 LRU 顺序淘汰"和"按页帧号快速增删"都高效，采用**链表 + 哈希集合**的组合：

- `std::list<frame_id_t> lru_list_`：双向链表，维护可替换页帧的先后顺序；
- `std::unordered_set<frame_id_t> lru_set_`：哈希集合，记录哪些页帧在链表中，用于 O(1) 判重与判断存在；
- `std::mutex latch_`：互斥锁，保证多线程访问的安全。

### 2. 各函数实现说明

每个函数开头都用 `std::lock_guard<std::mutex> lock(latch_)` 加锁，保证线程安全。

**`Victim(*frame_id)`**：淘汰最久未用的页帧。
- 若 `lru_list_` 为空，说明没有可替换的页，返回 `false`；
- 否则取链表尾部元素写入 `*frame_id`，并从 `lru_list_`（`pop_back`）和 `lru_set_`中删除该页帧，返回 `true`。

**`Pin(frame_id)`**：固定页帧，使其不可被替换。
- 若该页帧不在 `lru_set_` 中，直接返回，不做任何操作；
- 否则用 `lru_list_.remove(frame_id)` 从链表中移除，并从 `lru_set_` 中 `erase`。

**`Unpin(frame_id)`**：解除固定，使页帧重新可被替换。
- 若该页帧**已在** `lru_set_` 中，直接返回（避免重复加入）；
- 否则 `push_front` 到链表头部，并 `insert` 到 `lru_set_`。

**`Size()`**：返回当前可被替换的页帧数量，即 `lru_list_.size()`。

---

## 1.4 缓冲池管理 BufferPoolManager

### 1. 设计

`BufferPoolManager` 负责从 Disk Manager 取出数据页缓存在内存中，并在缓冲池空间不足时把脏页写回磁盘、腾出空间。

核心数据结构：

- `Page *pages_`：定长的页帧数组，下标即页帧号 `frame_id`。每个 `Page` 是一个容器，可在生命周期内承载不同的物理页；
- `unordered_map<page_id_t, frame_id_t> page_table_`：逻辑页号 → 页帧号的映射，用于 O(1) 判断某页是否已在缓冲池；
- `list<frame_id_t> free_list_`：尚未使用的空闲页帧列表；
- `Replacer *replacer_`：即 1.3 的 LRUReplacer，用于在没有空闲页帧时选出可淘汰的牺牲页；
- `recursive_mutex latch_`：可重入互斥锁。选用 `recursive_mutex` 是因为存在函数间的嵌套加锁。

### 2. 各函数实现说明

每个公共接口开头都用 `std::lock_guard<std::recursive_mutex> lock(latch_)` 加锁。

**`FetchPage(page_id)`**：取出逻辑页号对应的数据页。
- 先查 `page_table_`，**命中**则 `pin_count_++`、`replacer_->Pin` 后直接返回；
- 未命中则 `TryToFindFreePage()` 取一个页帧；若返回 `-1` 则返回 `nullptr`；
- 若该页帧上是脏页，先 `WritePage` 写回磁盘；
- 从 `page_table_` 删除旧页的映射、插入新页的映射，更新页元数据，`ReadPage` 从磁盘读入页内容，`Pin` 后返回。

**`NewPage(&page_id)`**：分配一个新数据页。
- **先** `TryToFindFreePage()` 确认有可用页帧，`-1` 则返回 `nullptr`；**之后**才调用 `AllocatePage()` 向磁盘申请逻辑页号；
```cpp
//关键顺序:先确认有可用页帧,再向磁盘申请逻辑页(否则磁盘页泄漏)
frame_id_t frame_id = TryToFindFreePage();
if (frame_id == -1) {
	return nullptr;
}
page_id = AllocatePage();
```
- 同样处理牺牲页的脏页写回与旧映射删除；
```cpp
Page *page = &pages_[frame_id];
if (page->IsDirty()) {
	disk_manager_->WritePage(page->GetPageId(), page->GetData());
	page->is_dirty_ = false;
}
page_table_.erase(page->GetPageId());  //删旧映射
```
- 重置页元数据、`ResetMemory()` 清零内存，登记 `page_table_[page_id] = frame_id`，`Pin` 后返回新页。
```cpp
page->page_id_ = page_id;
page->pin_count_ = 1;
page->is_dirty_ = false;
page->ResetMemory();
page_table_[page_id] = frame_id;
replacer_->Pin(frame_id);
return page;
```

**`UnpinPage(page_id, is_dirty)`**：解除固定一个数据页。
- 不在缓冲池中返回 `false`；
- `is_dirty` 为真则把 `is_dirty_` 置1；
- `pin_count_ <= 0` 属异常调用，返回 `false`；否则 `pin_count_--`，当引用计数归零时调用 `replacer_->Unpin`，使该页帧重新可被淘汰。

**`FlushPage(page_id)`**：将指定页写回磁盘。
- 不在缓冲池中返回 `false`；否则无论是否被固定都 `WritePage` 写回，并把 `is_dirty_` 置0。

**`DeletePage(page_id)`**：释放一个数据页。
- 不在缓冲池中视为已删除，返回 `true`；
- 若 `pin_count_ != 0`返回 `false`；
- 否则 `DeallocatePage` 通知磁盘回收、从 `page_table_` 删除映射、`replacer_->Pin` 把该页帧从替换器摘除、重置元数据，最后把页帧 `push_back` 回 `free_list_`。

**`FlushAllPages()`**：遍历 `page_table_`，对每个页调用 `FlushPage` 写回。

---

# 2. Record Manager

记录管理负责管理数据表中的所有记录，支持记录的插入、删除、更新与查找。本章先实现记录相关对象（列 Column、模式 Schema、行 Row）的序列化与反序列化，使其能够在内存对象与磁盘字节流之间相互转换；再借助**堆表 TableHeap** 将记录以无序堆的形式组织在数据页中，并提供遍历记录的**迭代器 TableIterator**。

## 2.1 序列化与反序列化

### 1. 设计

序列化把内存对象转成字节流，反序列化为其逆过程，二者共同实现持久化。

统一采用"偏移量游标"模式：用 `ofs` 记录已处理字节数、逐字段推进，函数返回前进的总字节数；读写用 `common/macros.h` 的 `MACH_WRITE_*` / `MACH_READ_*` 宏。

### 2. 各对象实现说明

**`Column`**：序列化字段依次为 `COLUMN_MAGIC_NUM`、name 长度 + name 内容、`type_`、`len_`、`table_ind_`、`nullable_`、`unique_`。
- 用一个指针 `p` 作游标，每写一个字段就 `p += sizeof(...)` 推进；定长字段（`type_`/`len_`/`table_ind_`/`nullable_`/`unique_`）直接写值，变长的 name 遵循"先写 4 字节长度、再写内容"的约定；函数返回 `p - buf` 即写入的总字节数：
```cpp
char *p = buf;
MACH_WRITE_UINT32(p, COLUMN_MAGIC_NUM);  p += sizeof(uint32_t);  //魔数,作字节布局锚点
MACH_WRITE_UINT32(p, name_.length());    p += sizeof(uint32_t);  //先长度
MACH_WRITE_STRING(p, name_);             p += name_.length();    //后内容
MACH_WRITE_TO(TypeId, p, type_);         p += sizeof(TypeId);
MACH_WRITE_UINT32(p, len_);              p += sizeof(uint32_t);
MACH_WRITE_UINT32(p, table_ind_);        p += sizeof(uint32_t);
MACH_WRITE_TO(bool, p, nullable_);       p += sizeof(bool);
MACH_WRITE_TO(bool, p, unique_);         p += sizeof(bool);
return p - buf;
```

- `GetSerializedSize` 与 `SerializeTo` 逐字段对齐：两个 `uint32`+ `name_.length()` + 其余定长字段之和：
```cpp
return sizeof(uint32_t) + sizeof(uint32_t) + name_.length() + sizeof(type_) + sizeof(len_) + sizeof(table_ind_) + sizeof(nullable_) + sizeof(unique_);
```

- 反序列化按相同顺序倒着读：先读魔数 `ASSERT` 校验，name 用 `std::string(buf + off, name_len)` 按长度还原，最后按类型选构造函数，返回字节数 `off`：
```cpp
//CHAR 带长度,其余不带
if (type == TypeId::kTypeChar) {
	column = new Column(column_name, type, len, table_ind, nullable, unique);
} else {
	column = new Column(column_name, type, table_ind, nullable, unique);
}
return off;
```

**`Schema`**：作为 `Column` 的容器，格式为 `SCHEMA_MAGIC_NUM` + 列数 + 各列依次序列化。
- 由于各列长度不定，序列化用 `p += column->SerializeTo(p)`、反序列化用 `off += Column::DeserializeFrom(...)` 以子对象返回的字节数推进；
- 反序列化收集出 `vector<Column *>` 后以 `new Schema(columns, true)` 构造，`is_manage_ = true` 表示由该 Schema 负责析构这些 Column，避免内存泄漏。

**`Row`**：格式为 `字段个数 | Null bitmap | 非 null 字段... 。
- Null bitmap 占 `⌈N/8⌉` 字节，第 i 位为 1 表示第 i 个字段为 null，位运算与第一章 `BitmapPage` 一致；
- 序列化只写非 null 字段；反序列化时，字段**类型**取自 `schema->GetColumn(i)->GetType()`，**是否为 null** 取自 bitmap，再调用 `Field::DeserializeFrom` 还原，新建的 `Field` 由该 `Row` 持有并在析构时释放；
- 边界情况：空 Row`GetSerializedSize` 与 `SerializeTo` 均返回 0；全 null 的非空 Row 仅占头部。


## 2.2  RowId

`RowId`是每条记录的唯一标识，本质是一个 64 位整数：高 32 位为记录所在数据页的 `page_id`，低 32 位为记录在该页中的槽位下标 `slot_num`。索引的叶结点中存储 Key→RowId 的映射，由 Key 查到 RowId；堆表中借助 RowId 的逻辑信息直接定位记录的物理位置。

## 2.3 堆表 TableHeap

### 1. 设计

堆表把记录以无序堆组织，由若干 `TablePage` 经双向链表连接，`first_page_id_` 为链表头，`INVALID_PAGE_ID` 表示链尾。页内槽式结构（Slotted-page）的字节操作由框架的 `TablePage` 提供，`TableHeap` 只负责页间编排——定位、翻页、申请新页，再调 `TablePage` 完成读写。每个操作的固定节奏为 `FetchPage → reinterpret_cast<TablePage*> → 加/解读写锁 → 调 TablePage 方法 → UnpinPage`。

### 2. 各函数实现说明

**`InsertTuple(&row, txn)`**：采用 First Fit 策略。
- 从 `first_page_id_` 沿链表逐页尝试 `page->InsertTuple(...)`，成功即以 `UnpinPage(..., true)` 收尾并返回 `true`；翻页时注意先 `GetNextPageId()` 取出后继再 `UnpinPage`；
- 所有现有页都放不下时，`NewPage` 申请新页、`Init` 初始化并链接到链表末尾，再在新页中插入。
```cpp
//所有页都放不下 → 在链表末尾挂一个新页再插入
page_id_t new_page_id;
auto new_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->NewPage(new_page_id));
new_page->Init(new_page_id, INVALID_PAGE_ID, log_manager_, txn);
new_page->WLatch();
new_page->InsertTuple(row, schema_, txn, lock_manager_, log_manager_);
new_page->WUnlatch();
buffer_pool_manager_->UnpinPage(new_page_id, true);
return true;
```

**`UpdateTuple(&row, rid, txn)`**：分两种情况。
- 将 `rid` 包装为 `Row old_row(rid)`，尝试原地更新；
- 原地更新成功则保持同一 slot，`row.SetRowId(rid)` 后返回；
- 原地放不下时，走"删旧 + 插新"：`MarkDelete` + `ApplyDelete` 删除旧记录，再 `InsertTuple(row)` 插入新记录，新 rid 经 `row` 返回。

**`ApplyDelete(rid, txn)`**：`FetchPage` 取页后调用 `page->ApplyDelete(...)` 物理删除该记录（兼容已 `MarkDelete` 与未标记两种情形），以 dirty 收尾。

**`GetTuple(*row, txn)`**：由 `row->rid_` 的 `page_id` 取页，加读锁后转发 `page->GetTuple(...)` 的返回值；只读不改，使用 `RLatch` 与 `UnpinPage(..., false)`。

**`Begin(txn)`**：从 `first_page_id_` 逐页用 `GetFirstTupleRid` 找第一条有效记录，找到即以该 rid 构造迭代器；整表无记录时返回与 `End()` 等价的无效迭代器。

**`End()`**：直接返回 `rid` 为 `INVALID_ROWID` 的"终点哨兵"迭代器，无需遍历。

## 2.4  TableIterator

### 1. 设计

`TableIterator` 用于上层顺序遍历堆表中的所有有效记录。头文件中其成员变量需自行设计，本实现采用三个成员：`TableHeap *table_heap_`（用于访问 `BufferPoolManager` 跨页前进）、`Row *row_`（缓存当前记录，同时携带当前 rid）、`Txn *txn_`。

### 2. 各函数实现说明

- **构造函数**：保存 `table_heap_`/`txn_`，`new Row(rid)`；若 rid 有效则 `GetTuple` 把记录内容读入缓存；
- **拷贝构造 / `operator=`**：对 `row_` 做深拷贝（`new Row(*other.row_)`），避免两个迭代器共享同一指针导致重复释放；析构中 `delete row_` 配对释放；
- **`operator==` / `!=`**：比较所属堆表与当前 rid 是否相同（均到尽头时 rid 同为 `INVALID_ROWID`）；
- **`operator*` / `->`**：返回当前 `row_`，对尽头迭代器以 `ASSERT` 拦截；
- **`operator++`**：先在本页用 `GetNextTupleRid` 找下一条；本页耗尽则沿 `GetNextPageId` 翻页并用 `GetFirstTupleRid` 取首条；
```cpp
cur_page->RLatch();
//先在本页找下一条
RowId next_rid;
bool found = cur_page->GetNextTupleRid(row_->GetRowId(), &next_rid);
//本页耗尽:沿 next_page 翻页,取下一页第一条
while (!found && cur_page->GetNextPageId() != INVALID_PAGE_ID) {
	auto next_page_id = cur_page->GetNextPageId();
	cur_page->RUnlatch();
	bpm->UnpinPage(cur_page->GetTablePageId(), false);
	cur_page = reinterpret_cast<TablePage *>(bpm->FetchPage(next_page_id));
	cur_page->RLatch();
	found = cur_page->GetFirstTupleRid(&next_rid);
}
cur_page->RUnlatch();
bpm->UnpinPage(cur_page->GetTablePageId(), false);
```
  找到则读入新记录，否则越过末条、把 rid 置为 `INVALID_ROWID` 变为 `End()`：
```cpp
delete row_;
if (found) {
	row_ = new Row(next_rid);
	table_heap_->GetTuple(row_, txn_);
} else {
	row_ = new Row(INVALID_ROWID);
}
return *this;
```
- **`operator++`**：保存副本、调用前缀自增、返回旧值。

# 3. Index Manager

索引管理负责数据表上索引的创建、删除以及基于索引的等值/范围查找，核心是一棵持久化在磁盘上的 **B+ 树**。本章先实现 B+ 树的三类结点页： 记录公共头部的 `BPlusTreePage`、存放"键 + 孩子指针"的中间结点 `BPlusTreeInternalPage`、存放"键 + RowId"的叶结点 `BPlusTreeLeafPage`。

## 3.1 B+ 树结点页总体设计

### 1. 三类页与内存布局

一个 B+ 树结点恰好占用一个数据页，分为头部和数据区两段：

- **`BPlusTreePage`**：字段有 `page_type_`、`key_size_`、`lsn_`、`size_`、`max_size_`、`parent_page_id_`、`page_id_`。
- **`BPlusTreeInternalPage`**：头部之后的数据区按 `| K0 | V0 | K1 | V1 | ... |` 紧密排列。`Vi` 指向的子树满足 `Ki ≤ key < K(i+1)`，因此查找从下标 1 开始。
- **`BPlusTreeLeafPage`**：数据区为 `| K0 | RowId0 | K1 | RowId1 | ... |`，键与 RowId **一一对应、从下标 0 起全部有效**；额外多一个 `next_page_id_` 把所有叶结点串成单向链表，供索引迭代器顺序遍历。

### 2. 变长键与定位宏

键长 `key_size_` 在运行时才确定，故每个"键值对"的字节宽度也是运行时量。三类页内统一用一组宏定位：

```
pairs_off = data_                              // 数据区起始
pair_size = GetKeySize() + sizeof(value)       // 一对的字节数；中间页 value 是 page_id_t，叶页是 RowId
key_off   = 0                                  // 键在一对中的偏移
val_off   = GetKeySize()                       // 值在一对中的偏移
```

第 `index` 个键/值的地址即 `pairs_off + index * pair_size + key_off / val_off`，`KeyAt`/`ValueAt`/`SetKeyAt`/`SetValueAt` 都据此用 `reinterpret_cast` + `memcpy` 读写。

## 3.2 BPlusTreePage

`BPlusTreePage` 提供被两类具体结点共享的头部读写：

- **`IsLeafPage()`**：`return page_type_ == IndexPageType::LEAF_PAGE;`。
- **`IsRootPage()`**：`return parent_page_id_ == INVALID_PAGE_ID;`。根结点的判定不靠页类型，而靠"有没有父结点"——根的 `parent_page_id_` 为 `INVALID_PAGE_ID(-1)`。
- **`SetPageType` / `GetMaxSize` / `SetMaxSize` / `GetParentPageId`**：直接读写对应成员。
- **`GetMinSize()`**：返回结点允许的最小键值对数，对根做特判：
  - 根且为叶：返回 1；
  - 根且为中间：返回 2；
  - 非根：返回 `(max_size_ + 1) / 2`，即向上取整的半满，是触发合并/借用的下界。

## 3.3 BPlusTreeInternalPage

### 1. 设计

中间结点存"键 + 孩子页号"，负责向下导航。搬移/合并/借用函数都要维护两条不变式：**K0 始终无效**、被移动孩子的 `parent_page_id_` 随之更新。

### 2. 各函数实现说明

**`Init`**：设置页类型为 `INTERNAL_PAGE`、`size_=0`，写入 page id / parent id / key size / max size。

**`Lookup(key)`**：在 `[1, size-1]` 上二分，返回应下降的孩子指针。`cmp==0` 返回 `ValueAt(mid)`；否则收敛到"最大的、键仍 `< key` 的下标"，末尾 `return ValueAt(right)` 不可省略，否则键落在两键之间时会返回未定义值。

**`PopulateNewRoot(old_value, new_key, new_value)`**：仅在树升高时新建根，写入 `V0`、`K1`、`V1`，`size_=2`。

**`InsertNodeAfter(old_value, new_key, new_value)`**：在值为 `old_value` 的指针之后插入新键值对。`ValueIndex` 定位后，用 `memmove` 把后续整体右移一格腾位。

**`MoveHalfTo` / `CopyNFrom`**：分裂时把后半部分搬给兄弟。`MoveHalfTo` 以 `half = size/2` 为界调用 `CopyNFrom`.`CopyNFrom` 先 `memcpy` 到末尾、更新 size，再放入新的孩子。

**`Remove(index)` / `RemoveAndReturnOnlyChild`**：前者把 `index` 之后的对整体左移补齐、`IncreaseSize(-1)`；后者用于 `AdjustRoot`，结点只剩 V0 时取出该唯一孩子并清空。

**`MoveAllTo(recipient, middle_key, bpm)`**：合并到左兄弟。先 `SetKeyAt(0, middle_key)` 用父分隔键填上无效 K0，再整体 `CopyNFrom` 给 recipient。

**`MoveFirstToEndOf` / `CopyLastFrom`**：把本结点第一个孩子接到左兄弟末尾。`middle_key` 作为新键随孩子一起追加，`CopyLastFrom` 在末尾写对、认领孩子；随后 `Remove(0)` 删除本结点首对。

**`MoveLastToFrontOf` / `CopyFirstFrom`**：把本结点最后一个孩子插到右兄弟最前。先 `recipient->SetKeyAt(0, middle_key)` 处理右兄弟原本无效的 K0，再 `CopyFirstFrom` 整体右移并在 0 号位放入孩子、认领。

## 3.4 叶结点 BPlusTreeLeafPage

### 1. 设计

叶结点存"键 + RowId"，是真正承载索引数据的层。

与中间结点的三点差异让它更简单：

1. **没有无效 K0**：键值从下标 0 起全部有效，借用/合并**不需要 middle_key**；
2. **不认领孩子**：value 是 RowId 而非页号，搬动不碰缓冲池（搬移函数也无需 `BufferPoolManager` 参数）；
3. **多了 `next_page_id_`**：合并时须接上叶链表，否则迭代器遍历断链。

### 2. 各函数实现说明

叶页的搬移/查找/插删与中间页**结构同构**，故仅列出叶页**特有**或需注意的几点，其余与中间页一致，差别只在**不传 middle_key、不认领孩子**。

- **`Init`**：两个易漏点——必须 `SetKeySize(key_size)`；`next_page_id_` 初始化为 `INVALID_PAGE_ID` 而非 0。
- **`Lookup(key, &value)`**：`KeyIndex` 返回"第一个 ≥ key"的下标，**还需再比一次确认相等**才算命中。
- **`MoveAllTo(recipient)`**：合并到左兄弟时，除 `CopyNFrom` 外关键一步是 `recipient->SetNextPageId(GetNextPageId())`，把后继接上以维护叶链表。

## 3.5 B+ 树主体 BPlusTree

### 1. 设计

`BPlusTree` 把三类结点页组装成一棵可持久化、可动态增长/收缩的 B+ 树，对外提供 `Insert`/`Remove`/`GetValue` 及范围扫描迭代器：

- **每次取结点页都是 `reinterpret_cast<...>(FetchPage(id)->GetData())`，用完必须 `UnpinPage`**；
- **`root_page_id_` 的变更必须经 `UpdateRootPageId` 持久化**到 `INDEX_ROOTS_PAGE_ID`；第一次建树传 `insert_record=1`，后续根变化传 `0`；
- **`size_` 即指针/键值对数量**，结点容量 `max_size_` 在构造时按键长反推：`(PAGE_SIZE - 页头) / (键长 + 值长) - 1`，末尾 `-1` 给"先插入后分裂"留一个临时溢出位，使分裂条件统一为 `GetSize() > GetMaxSize()`。

### 2. 各函数实现说明

**`BPlusTree(...)` 构造**：若传入的 `leaf_max_size`/`internal_max_size` 为 `UNDEFINED_SIZE(0)`，按上述公式据 `processor_.GetKeySize()` 计算；随后从 roots 页读取本 `index_id_` 已记录的 `root_page_id_`，从而支持索引重新打开。

**`GetValue(key, &result)`**：`FindLeafPage` 下降到目标叶页，`leaf->Lookup` 命中则把 RowId 推入 `result`，unpin 后返回是否命中。

**`FindLeafPage(key, page_id, leftMost)`**：从根开始，每层 `internal->Lookup(key)` 选孩子，下降前先 unpin 当前页，直到叶页。
- 取页：
- 一路下降到叶页，保证任意时刻只 pin 一页；叶页返回时仍 pinned，交调用方 unpin：
```cpp
while (!node->IsLeafPage()) {
	auto *internal = reinterpret_cast<InternalPage *>(node);
	//leftMost 恒取最左孩子(给 Begin() 找最小 key);否则按 key 导航
	page_id_t child_id = leftMost ? internal->ValueAt(0) : internal->Lookup(key, processor_);
	buffer_pool_manager_->UnpinPage(cur_id, false);  //下降前先unpin当前层
	cur_id = child_id;
	page = buffer_pool_manager_->FetchPage(cur_id);
	node = reinterpret_cast<BPlusTreePage *>(page->GetData());
}
return page;
```

**`Insert` → `StartNewTree` / `InsertIntoLeaf`**：空树时 `StartNewTree` 申请新页、`Init` 为根叶页、`UpdateRootPageId(1)` 持久化、插入首个键值对；非空时转 `InsertIntoLeaf`：先 `Lookup` 查重，插入后若 `GetSize() > GetMaxSize()` 则 `Split` 出新叶，并把**新叶第一个 key 拷贝上提**（leaf 是 copy-up）交给 `InsertIntoParent`。

**`Split`**：申请新页、`Init`、`MoveHalfTo` 搬走后半。

**`InsertIntoParent(old_node, key, new_node)`**：若 `old_node` 是根，新建一个根，两孩子认新根为父，`UpdateRootPageId(0)`；否则取父页、`InsertNodeAfter` 在 `old_node` 指针后插入，父页若也超容量则**递归** `Split(parent)` + `InsertIntoParent` 向上分裂。内部页分裂上提的 key 是 **push-up**（新页 `KeyAt(0)` 升上去、其在新页中的槽位变为无效 K0）。

```cpp
void BPlusTree::InsertIntoParent(BPlusTreePage *old_node, GenericKey *key,
                                 BPlusTreePage *new_node, Txn *transaction) {
  //old_node 是根:树长高一层,新建一个根把两个孩子接上去
  if (old_node->IsRootPage()) {
    page_id_t new_root_id;
    Page *new_root_page = buffer_pool_manager_->NewPage(new_root_id);
    if (new_root_page == nullptr) throw std::bad_alloc();
    auto *new_root = reinterpret_cast<InternalPage *>(new_root_page->GetData());
    new_root->Init(new_root_id, INVALID_PAGE_ID, processor_.GetKeySize(), internal_max_size_);
    new_root->PopulateNewRoot(old_node->GetPageId(), key, new_node->GetPageId());
    old_node->SetParentPageId(new_root_id);   //两孩子认新根为父
    new_node->SetParentPageId(new_root_id);
    root_page_id_ = new_root_id;
    UpdateRootPageId(0);                       //根变了,更新记录
    buffer_pool_manager_->UnpinPage(new_root_id, true);
    return;
  }

  //否则把 key 插到 old_node 的父页里 old_node 指针之后
  page_id_t parent_id = old_node->GetParentPageId();
  auto *parent = reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(parent_id)->GetData());
  new_node->SetParentPageId(parent_id);
  parent->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());

  //父页也撑爆了 → 继续分裂父页,把它的新首键递归上提(push-up)
  if (parent->GetSize() > internal_max_size_) {
    InternalPage *new_parent = Split(parent, transaction);
    InsertIntoParent(parent, new_parent->KeyAt(0), new_parent, transaction);
    buffer_pool_manager_->UnpinPage(new_parent->GetPageId(), true);
  }
  buffer_pool_manager_->UnpinPage(parent_id, true);
}
```

**`Remove` → `CoalesceOrRedistribute`**：`Remove` 找叶、`RemoveAndDeleteRecord`，删成功后调 `CoalesceOrRedistribute` 并按返回值决定是否 `DeletePage`。

`CoalesceOrRedistribute<N>`（叶/内部通用模板）依次判断：1. 是根 → `AdjustRoot`；2. `size ≥ min` 未下溢 → `false`；3. 下溢则取父、定位 `index`、选兄弟；4. 两者之和 `> max` 则 `Redistribute` 借一个，否则 `Coalesce` 合并。

```cpp
template <typename N>
bool BPlusTree::CoalesceOrRedistribute(N *&node, Txn *transaction) {
  if (node->IsRootPage()) return AdjustRoot(node);          //根单独处理
  if (node->GetSize() >= node->GetMinSize()) return false;  //没下溢,不用动

  //下溢:取父、定位 node、选兄弟(默认左兄弟,node 最左时才取右兄弟)
  page_id_t parent_id = node->GetParentPageId();
  auto *parent = reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(parent_id)->GetData());
  int index = parent->ValueIndex(node->GetPageId());
  int sibling_index = (index == 0) ? 1 : index - 1;
  page_id_t sibling_page_id = parent->ValueAt(sibling_index);
  auto *sibling = reinterpret_cast<N *>(buffer_pool_manager_->FetchPage(sibling_page_id)->GetData());

  //兄弟 + 自己塞不进一页 → 借一个(Redistribute),不删任何页
  if (sibling->GetSize() + node->GetSize() > node->GetMaxSize()) {
    Redistribute(sibling, node, index);
    buffer_pool_manager_->UnpinPage(sibling_page_id, true);
    buffer_pool_manager_->UnpinPage(parent_id, true);
    return false;
  }

  //塞得下 → 合并,统一"右页并入左页",由此厘清谁被清空、谁来删它
  bool node_should_delete, parent_should_delete;
  if (index == 0) {
    //node 最左,sibling 在右:sibling 并入 node。被清空的是 sibling(本函数 pin → 本函数删)
    parent_should_delete = Coalesce(node, sibling, parent, 1, transaction);
    buffer_pool_manager_->UnpinPage(sibling_page_id, true);
    buffer_pool_manager_->DeletePage(sibling_page_id);
    node_should_delete = false;  //node 存活,不用调用方删
  } else {
    //sibling 在左,node 在右:node 并入 sibling。被清空的是 node(交给最外层 Remove 删)
    parent_should_delete = Coalesce(sibling, node, parent, index, transaction);
    buffer_pool_manager_->UnpinPage(sibling_page_id, true);
    node_should_delete = true;
  }

  //合并使父页少一项,递归可能判定父也该删
  buffer_pool_manager_->UnpinPage(parent_id, true);
  if (parent_should_delete) buffer_pool_manager_->DeletePage(parent_id);
  return node_should_delete;
}
```

**`Coalesce`（leaf / internal）**：把右页并入左页，`parent->Remove(index)` 删分隔项，再**递归** `CoalesceOrRedistribute(parent)` 处理父页下溢。

**`Redistribute`（leaf / internal）**：`index==0` 时向右兄弟借第一个接到 `node` 末尾，否则向左兄弟借最后一个接到 `node` 头部，并更新父结点对应分隔键。内部页是**三方旋转**：父分隔键下放进 `node`、兄弟边界 key 升上去替换父分隔键。下面是内部页版本，两个分支里"升上去的 key"的读取**时机不同**——即 `GenericKey *` 指针的时效性问题：

```cpp
void BPlusTree::Redistribute(InternalPage *neighbor_node, InternalPage *node, int index) {
  page_id_t parent_id = node->GetParentPageId();
  auto *parent = reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(parent_id)->GetData());

  if (index == 0) {
    //node 最左,neighbor 是右兄弟(父下标 1):父第 1 项下放,借 neighbor 第一个孩子到 node 末尾
    neighbor_node->MoveFirstToEndOf(node, parent->KeyAt(1), buffer_pool_manager_);
    //MoveFirstToEndOf 内部 Remove(0) 已把 neighbor 原 K1 左移到槽 0 → 搬移【之后】读它当新分界
    parent->SetKeyAt(1, neighbor_node->KeyAt(0));
  } else {
    //neighbor 左兄弟,node 在右:neighbor 最后一个 key 要升上去当新分界
    //MoveLastToFrontOf 不左移 neighbor,但该 key 的内存会被覆盖 → 搬移【之前】先抓住
    GenericKey *up_key = neighbor_node->KeyAt(neighbor_node->GetSize() - 1);
    //父第 index 项下放,借 neighbor 最后一个孩子到 node 头部
    neighbor_node->MoveLastToFrontOf(node, parent->KeyAt(index), buffer_pool_manager_);
    parent->SetKeyAt(index, up_key);
  }
  buffer_pool_manager_->UnpinPage(parent_id, true);
}
```

**`AdjustRoot`**：处理根的退化——1. 根为内部页且只剩一个孩子（`size==1`）时让该唯一孩子成为新根；2. 根为叶页且删空（`size==0`）时整棵树清空，`root_page_id_` 置 `INVALID`。两种情况都返回 `true` 表示旧根应被删除。

**`Destroy`**：后序遍历递归释放所有页（先删孩子再删自己），顶层调用末尾重置 `root_page_id_` 并从 roots 页删记录。

## 3.6 索引迭代器 IndexIterator

### 1. 设计

`IndexIterator` 沿叶结点的 `next_page_id_` 单向链表有序遍历全部键值对，用于范围扫描。成员为 `current_page_id`（当前叶页号）、`page`（当前叶页指针）、`item_index`（页内下标）、`buffer_pool_manager`。

### 2. 各函数实现说明

- **`BPlusTree::Begin()`**：`FindLeafPage(nullptr, ?, leftMost=true)` 找最左叶页，从 `item_index=0` 构造迭代器；`FindLeafPage` 与迭代器构造各 `Fetch` 一次，故 `Begin` 中先 unpin 一次抵消，使该页净 pin 数为 1（迭代器持有）；
- **`BPlusTree::Begin(key)`**：`FindLeafPage(key)` 定位叶页，`KeyIndex(key)` 求第一个 `≥ key` 的下标作为起点；
- **`BPlusTree::End()`**：返回默认构造的迭代器（`current_page_id == INVALID_PAGE_ID`），表示"最后一个之后"；
- **`operator*`**：返回 `page->GetItem(item_index)`，即当前 `{GenericKey *, RowId}` 键值对；
- **`operator++`**：页内还有则 `item_index++`；否则取 `next_page_id_`、unpin 当前页：无后继则退化为 end，有则 `FetchPage` 新页、`item_index` 归零；
- **`operator==`/`!=`**：比较 `current_page_id` 与 `item_index`，使 `++` 越过末条后恰好等于 `End()`。

---

# 4. Catalog Manager

Catalog Manager 负责维护数据库中的所有模式信息，包括表定义、列定义、索引定义，以及这些元信息在数据库文件中的持久化位置。本章在前面 Disk、Buffer、Record、Index 模块的基础上，实现 `CatalogMeta`、`TableMetadata`、`IndexMetadata` 的序列化管理，并通过 `CatalogManager` 向上层 Planner 和 Executor 提供表与索引的创建、查询、删除接口。

## 4.1 目录元信息 CatalogMeta

### 1. 设计

`CatalogMeta` 存储在逻辑页 `CATALOG_META_PAGE_ID` 中，用来记录每张表和每个索引的元信息页号。核心成员包括：

- `table_meta_pages_`：`table_id -> table metadata page_id`；
- `index_meta_pages_`：`index_id -> index metadata page_id`。

每张表和每个索引都单独分配一个数据页保存元信息，因此 CatalogMeta 只需要维护这些元信息页的索引目录。

### 2. 各函数实现说明

**`CatalogMeta::SerializeTo(buf)`**：将 CatalogMeta 写入 catalog meta page。
- 首先写入魔数 `CATALOG_METADATA_MAGIC_NUM`，用于反序列化时校验；
- 写入表数量和索引数量；
- 依次写入所有 `{table_id, page_id}` 和 `{index_id, page_id}`。

**`CatalogMeta::DeserializeFrom(buf)`**：从 catalog meta page 恢复目录元信息。
- 先读取魔数并 `ASSERT` 校验；
- 根据表数量和索引数量循环读取映射关系；
- 构造新的 `CatalogMeta` 对象并返回。

**`CatalogMeta::GetSerializedSize()`**：计算 CatalogMeta 序列化后的字节数。

```cpp
return 3 * sizeof(uint32_t) +
       (table_meta_pages_.size() + index_meta_pages_.size()) * 2 * sizeof(uint32_t);
```

其中前 3 个 `uint32_t` 分别是 magic number、表数量、索引数量；每条表/索引映射占两个 `uint32_t`。

### 3. 表和索引元信息大小计算

实验要求中除了 `CatalogMeta::GetSerializedSize()`，还需要实现 `TableMetadata::GetSerializedSize()` 与 `IndexMetadata::GetSerializedSize()`。这两个函数的作用是：在真正序列化前，提前计算元信息写入磁盘页后会占用多少字节，用于检查其是否能放入一个 4KB Page，并保证 `SerializeTo` 中写入的字段总长度与预期一致。

**`TableMetadata::GetSerializedSize()`**：计算单张表元信息的序列化长度。

`TableMetadata` 的序列化字段顺序为：

```
TABLE_METADATA_MAGIC_NUM
table_id_
table_name_ length + table_name_ data
root_page_id_
schema_
```

因此其大小计算为：

```cpp
return 4 + 4 + MACH_STR_SERIALIZED_SIZE(table_name_) + 4 + schema_->GetSerializedSize();
```

逐项含义如下：

- 第一个 `4`：`TABLE_METADATA_MAGIC_NUM`，用于反序列化时校验页中保存的确实是表元信息；
- 第二个 `4`：`table_id_`；
- `MACH_STR_SERIALIZED_SIZE(table_name_)`：表名的序列化大小。字符串采用"4 字节长度 + 字符串内容"的格式，因此该宏等价于 `4 + table_name_.length()`；
- 第三个 `4`：`root_page_id_`，即该表堆表首页页号；
- `schema_->GetSerializedSize()`：表结构的序列化长度，包括所有列的名称、类型、长度、列下标、nullable、unique 等信息。

该函数和 `TableMetadata::SerializeTo()` 的字段写入顺序一一对应。`SerializeTo()` 中也会用：

```cpp
uint32_t ofs = GetSerializedSize();
ASSERT(ofs <= PAGE_SIZE, "Failed to serialize table info.");
```

保证表元信息可以完整放入一个数据页。

**`IndexMetadata::GetSerializedSize()`**：计算单个索引元信息的序列化长度。

`IndexMetadata` 的序列化字段顺序为：

```
INDEX_METADATA_MAGIC_NUM
index_id_
index_name_ length + index_name_ data
table_id_
index key count
key_map_[0], key_map_[1], ...
```

因此其大小计算为：

```cpp
return 4 + 4 + MACH_STR_SERIALIZED_SIZE(index_name_) + 4 + 4 + key_map_.size() * 4;
```

逐项含义如下：

- 第一个 `4`：`INDEX_METADATA_MAGIC_NUM`，用于反序列化校验；
- 第二个 `4`：`index_id_`；
- `MACH_STR_SERIALIZED_SIZE(index_name_)`：索引名大小，即 `4 + index_name_.length()`；
- 第三个 `4`：`table_id_`，表示该索引属于哪张表；
- 第四个 `4`：索引包含的列数，即 `key_map_.size()`；
- `key_map_.size() * 4`：每个索引列在原表 schema 中的下标，一个下标用 `uint32_t` 存储，占 4 字节。

例如某索引建立在两列上，`key_map_ = {0, 2}`，则最后一部分占 `2 * 4 = 8` 字节，分别记录第 0 列和第 2 列作为索引 key。恢复索引时，`IndexInfo::Init()` 会根据这个映射调用 `Schema::ShallowCopySchema` 构造索引 key schema。

## 4.2 表元信息 TableMetadata

### 1. 设计

`TableMetadata` 保存单张表的定义信息，包括：

- `table_id_`：表 id；
- `table_name_`：表名；
- `root_page_id_`：堆表首页页号；
- `schema_`：表的列定义。

`TableInfo` 是运行时对象，内部维护 `TableMetadata *table_meta_` 和 `TableHeap *table_heap_`，供执行器直接访问表堆。

### 2. 各函数实现说明

**`TableMetadata::SerializeTo(buf)`**：依次写入 magic number、table id、table name、table heap root page id、schema。

**`TableMetadata::DeserializeFrom(buf, table_meta)`**：按相同顺序读取字段，并调用 `TableSchema::DeserializeFrom` 恢复表结构。

**`TableMetadata::GetSerializedSize()`**：由表名和 schema 的序列化大小共同决定：

```cpp
return 4 + 4 + MACH_STR_SERIALIZED_SIZE(table_name_) + 4 + schema_->GetSerializedSize();
```

这里包括 magic number、table id、表名、root page id 和 schema。

## 4.3 索引元信息 IndexMetadata 与 IndexInfo

### 1. 设计

`IndexMetadata` 保存索引的定义信息：

- `index_id_`：索引 id；
- `index_name_`：索引名；
- `table_id_`：所属表 id；
- `key_map_`：索引列在表 schema 中的下标映射。

`IndexInfo` 是运行时对象，除元信息外，还维护：

- `key_schema_`：索引 key 的 schema；
- `index_`：具体索引对象，本实验中为 `BPlusTreeIndex`。

### 2. 各函数实现说明

**`IndexMetadata::GetSerializedSize()`**：计算索引元信息大小：

```cpp
return 4 + 4 + MACH_STR_SERIALIZED_SIZE(index_name_) + 4 + 4 + key_map_.size() * 4;
```

字段依次为 magic number、index id、index name、table id、索引列数量和每个列下标。

**`IndexInfo::Init(meta_data, table_info, bpm)`**：根据索引元信息和所属表创建运行时索引对象。
- 保存 `meta_data_`；
- 调用 `Schema::ShallowCopySchema(table_info->GetSchema(), key_map)` 创建索引 key schema；
- 调用 `CreateIndex` 创建 `BPlusTreeIndex`。

这里必须使用浅拷贝，因为索引 schema 中的列对象与原表 schema 共享，不应重复释放。

## 4.4 CatalogManager

### 1. 初始化与加载

`CatalogManager` 构造时根据 `init` 分两种情况：

**新建数据库（`init = true`）**：
- 创建新的 `CatalogMeta`；
- 初始化 `next_table_id_` 和 `next_index_id_`；
- 将空的 CatalogMeta 刷入 `CATALOG_META_PAGE_ID`。

**打开已有数据库（`init = false`）**：
- 从 `CATALOG_META_PAGE_ID` 读取并反序列化 CatalogMeta；
- 根据 `table_meta_pages_` 调用 `LoadTable` 恢复所有表；
- 根据 `index_meta_pages_` 调用 `LoadIndex` 恢复所有索引。

### 2. 表管理

**`CreateTable(table_name, schema, txn, table_info)`**：
- 若表名已存在，返回 `DB_TABLE_ALREADY_EXIST`；
- 分配新的 `table_id`；
- 使用 `Schema::DeepCopySchema(schema)` 深拷贝表结构，避免外部 `shared_ptr<Schema>` 析构后悬空；
- 调用 `TableHeap::Create` 创建堆表首页；
- 创建并序列化 `TableMetadata` 到新分配的元信息页；
- 在 `table_names_`、`tables_` 和 `catalog_meta_->table_meta_pages_` 中登记；
- 调用 `FlushCatalogMetaPage()` 持久化目录页。

**`GetTable(table_name, table_info)`**：通过 `table_names_` 找到 `table_id`，再到 `tables_` 中取出 `TableInfo`。

**`GetTables(tables)`**：遍历 `tables_`，返回当前数据库中所有表。

**`DropTable(table_name)`**：
- 找到目标表；
- 先删除该表上的所有索引；
- 调用 `TableHeap::DeleteTable()` 释放堆表数据页；
- 删除表元信息页；
- 从内存映射和 CatalogMeta 中移除表记录，并刷新 CatalogMeta。

### 3. 索引管理

**`CreateIndex(table_name, index_name, index_keys, txn, index_info, index_type)`**：
- 检查表是否存在，索引是否重名；
- 根据列名在表 schema 中查找列下标，构造 `key_map`；
- 分配新的 `index_id` 和索引元信息页；
- 创建 `IndexInfo` 和内部 `BPlusTreeIndex`；
- 遍历表中已有记录，把旧数据回填到新建索引中，保证"对已有表创建索引"后可以立即通过索引查询旧记录；
- 更新 `index_names_`、`indexes_` 和 `catalog_meta_->index_meta_pages_`。

**`GetIndex(table_name, index_name, index_info)`**：通过二级映射 `table_name -> index_name -> index_id` 获取索引对象。

**`GetTableIndexes(table_name, indexes)`**：返回某张表上的全部索引。

**`DropIndex(table_name, index_name)`**：
- 找到索引对象；
- 调用 `index_->Destroy()` 释放 B+ 树页；
- 从 index roots page 删除索引根记录；
- 删除索引元信息页和内存映射；
- 刷新 CatalogMeta。

---

# 5. Planner and Executor

Planner 与 Executor 负责把 Parser 生成的语法树转换成执行计划，并通过火山模型执行计划树。本章主要实现 SQL 的查询、插入、删除、更新执行链路，并补全数据库交互命令中的表和索引操作。

## 5.1 Planner

### 1. 设计

Planner 的入口为 `PlanQuery(ast)`，根据语法树根节点类型构造不同的 Statement，再生成对应 PlanNode：

- `kNodeSelect` → `SelectStatement` → `SeqScanPlanNode` 或 `IndexScanPlanNode`；
- `kNodeInsert` → `InsertStatement` → `ValuesPlanNode + InsertPlanNode`；
- `kNodeDelete` → `DeleteStatement` → `SeqScanPlanNode + DeletePlanNode`；
- `kNodeUpdate` → `UpdateStatement` → `SeqScanPlanNode + UpdatePlanNode`。

Statement 层负责把语法树转换为更容易执行的结构，例如表名、列列表、谓词表达式、插入值列表、更新列映射等。

### 2. Select 计划生成

`PlanSelect` 首先根据 select 列表构造输出 schema：

```cpp
auto out_schema = MakeOutputSchema(statement->column_list_);
```

随后检查该表上是否存在可用索引：
- 只考虑单列索引；
- 如果索引列出现在 where 条件中，则认为该索引可用于扫描；
- 若没有可用索引，或者 where 中存在 `or`，则退化为顺序扫描；
- 否则生成 `IndexScanPlanNode`。

这样可以在简单等值或范围查询中使用 B+ 树索引加速，而复杂 `or` 条件继续使用顺序扫描保证正确性。

### 3. Insert/Delete/Update 计划生成

**`PlanInsert`**：把原始 value 表达式包装为 `ValuesPlanNode`，再作为 `InsertPlanNode` 的子节点。

**`PlanDelete`**：先生成带谓词的 `SeqScanPlanNode` 找到待删除记录，再包装为 `DeletePlanNode`。

**`PlanUpdate`**：同样先通过 `SeqScanPlanNode` 找到满足 where 条件的记录，再由 `UpdatePlanNode` 保存更新列和更新表达式。

## 5.2 表达式系统

### 1. 常量与列表达式

`ConstantValueExpression` 用于表示 SQL 中的常量，例如 `1`、`2.5`、`"abc"`、`null`。

`ColumnValueExpression` 用于从当前行中读取指定列：

```cpp
row->GetField(col_idx)
```

### 2. 比较表达式

`ComparisonExpression` 支持：

- `=`
- `<>`
- `<`
- `<=`
- `>`
- `>=`
- `is`
- `not`

执行时先分别计算左右子表达式，再调用 `Field` 的比较函数返回布尔结果。

### 3. 逻辑表达式

`LogicExpression` 支持 `and` 与 `or`，用于组合多个 where 条件。其返回值仍使用 `Field(kTypeInt, 0/1)` 表示布尔结果，便于执行器统一判断。

## 5.3 Executor

### 1. SeqScanExecutor

`SeqScanExecutor` 通过 `TableHeap::Begin()` 获取表迭代器，逐条扫描记录：

- 若没有谓词，直接返回当前行；
- 若有谓词，则调用 `predicate->Evaluate(row)` 判断是否满足；
- 若输出 schema 与表 schema 不完全一致，则调用 `TupleTransfer` 只抽取 select 列。

核心流程为：

```cpp
while (iterator_ != table_info_->GetTableHeap()->End()) {
  auto p_row = &(*iterator_);
  if (predicate != nullptr && !predicate->Evaluate(p_row).CompareEquals(Field(kTypeInt, 1))) {
    iterator_++;
    continue;
  }
  *rid = iterator_->GetRowId();
  TupleTransfer(...);
  iterator_++;
  return true;
}
return false;
```

### 2. IndexScanExecutor

`IndexScanExecutor` 首先根据 where 谓词递归收集候选 RowId：

- 比较表达式：找到条件列对应的索引，调用 `Index::ScanKey`；
- 逻辑表达式：对子表达式分别扫描，并对结果取交集；
- 若计划标记 `need_filter_`，则在取出记录后继续用完整谓词过滤一次。

这样可以在部分条件使用索引时，仍保证最终结果满足完整 where 条件。

### 3. ValuesExecutor

`ValuesExecutor` 用于 insert 语句，把 `ValuesPlanNode` 中保存的常量表达式求值后构造一行 `Row`：

```cpp
for (auto expr : exprs) {
  values.emplace_back(expr->Evaluate(nullptr));
}
*row = Row{values};
```

### 4. InsertExecutor

`InsertExecutor` 从子执行器读取待插入行：
- 先检查所有索引上是否存在重复 key；
- 调用 `TableHeap::InsertTuple` 插入表堆；
- 插入成功后，为该表上的每个索引生成 key 并调用 `InsertEntry` 更新索引。

### 5. DeleteExecutor

`DeleteExecutor` 从子执行器读取满足条件的记录：
- 调用 `TableHeap::MarkDelete` 标记删除；
- 对该表上的所有索引，生成 key 并调用 `RemoveEntry` 删除索引项。

### 6. UpdateExecutor

`UpdateExecutor` 从子执行器读取待更新记录：
- `GenerateUpdatedTuple` 根据 update 列映射生成新行；
- 调用 `TableHeap::UpdateTuple` 更新表堆；
- 对每个索引，先删除旧 key，再插入新 key。

在实现过程中，`Row::DeserializeFrom` 改为在反序列化前清空已有字段，避免更新旧记录时向非空 Row 中反序列化导致断言失败。

## 5.4 ExecuteEngine 中的 DDL 命令

除 DML 执行器外，还补全了以下交互命令：

**`ExecuteCreateTable`**：
- 解析表名、列定义、主键定义；
- 构造 `Column` 和 `Schema`；
- 调用 `CatalogManager::CreateTable`；
- 对主键列自动创建 B+ 树索引。

**`ExecuteDropTable`**：调用 `CatalogManager::DropTable` 删除表及其索引。

**`ExecuteCreateIndex`**：解析索引名、表名、索引列和索引类型，调用 `CatalogManager::CreateIndex`。

**`ExecuteDropIndex`**：遍历所有表，找到指定索引后调用 `CatalogManager::DropIndex`。

**`ExecuteShowIndexes`**：遍历所有表并输出其索引信息。

**`ExecuteExecfile`**：读取文件中的 SQL，逐条交给 Parser 和 ExecuteEngine 执行。

**`ExecuteQuit`**：返回 `DB_QUIT`，由主循环退出。

---

# 6. Recovery Manager

Recovery Manager 负责根据内存日志模拟数据库恢复流程。本实验不考虑日志落盘和页级恢复，而是使用一个 `unordered_map<KeyType, ValType>` 模拟 KV Database，并通过内存中的 `LogRec` 完成 Redo 与 Undo。

## 6.1 日志结构 LogRec

### 1. 设计

日志类型包括：

- `kInsert`
- `kDelete`
- `kUpdate`
- `kBegin`
- `kCommit`
- `kAbort`

`LogRec` 被设计为一个可表示所有日志类型的通用结构：

- `type_`：日志类型；
- `txn_id_`：事务 id；
- `lsn_`：当前日志 LSN；
- `prev_lsn_`：同一事务上一条日志的 LSN；
- `old_key_ / old_val_`：删除或更新前的数据；
- `new_key_ / new_val_`：插入或更新后的数据。

同时用两个静态变量辅助测试：

- `prev_lsn_map_`：记录每个事务最后一条日志的 LSN；
- `next_lsn_`：下一个可分配 LSN。

为了避免头文件中定义静态变量导致不同测试目标出现多份状态，静态成员的定义被放在 `src/recovery/log_rec.cpp` 中。

### 2. 日志创建函数

**`CreateInsertLog(txn_id, ins_key, ins_val)`**：
- 创建 `kInsert` 日志；
- `new_key_ = ins_key`，`new_val_ = ins_val`。

**`CreateDeleteLog(txn_id, del_key, del_val)`**：
- 创建 `kDelete` 日志；
- `old_key_ = del_key`，`old_val_ = del_val`。

**`CreateUpdateLog(txn_id, old_key, old_val, new_key, new_val)`**：
- 创建 `kUpdate` 日志；
- 同时记录更新前后的 key/value。

**`CreateBeginLog(txn_id)`**：
- 创建事务开始日志；
- `prev_lsn_` 固定为 `INVALID_LSN`；
- 将该事务最后一条 LSN 更新为 begin log 的 LSN。

**`CreateCommitLog(txn_id)`** 与 **`CreateAbortLog(txn_id)`**：
- 创建提交或回滚日志；
- `prev_lsn_` 指向该事务此前最后一条日志；
- 事务结束后从 `prev_lsn_map_` 中移除该事务。

## 6.2 CheckPoint

`CheckPoint` 表示某一时刻数据库已经持久化的完整状态，包括：

- `checkpoint_lsn_`：检查点对应的 LSN；
- `active_txns_`：检查点时仍活跃的事务及其最后一条 LSN；
- `persist_data_`：检查点时已经持久化的 KV 数据。

`RecoveryManager::Init(checkpoint)` 会把这些内容复制到恢复管理器中：

```cpp
persist_lsn_ = last_checkpoint.checkpoint_lsn_;
active_txns_ = last_checkpoint.active_txns_;
data_ = last_checkpoint.persist_data_;
```

## 6.3 RedoPhase

### 1. 设计

Redo 从 checkpoint 之后开始扫描日志。对于每条日志，根据类型更新数据库状态和活跃事务表。

### 2. 各日志处理

**`kBegin`**：将事务加入 `active_txns_`。

**`kInsert`**：执行插入：

```cpp
data_[log->new_key_] = log->new_val_;
active_txns_[log->txn_id_] = log->lsn_;
```

**`kDelete`**：执行删除：

```cpp
data_.erase(log->old_key_);
active_txns_[log->txn_id_] = log->lsn_;
```

**`kUpdate`**：执行更新：

```cpp
data_.erase(log->old_key_);
data_[log->new_key_] = log->new_val_;
active_txns_[log->txn_id_] = log->lsn_;
```

**`kCommit`**：事务已提交，从 `active_txns_` 中删除。

**`kAbort`**：事务已经回滚，需要沿 `prev_lsn_` 链撤销该事务已做修改，然后从 `active_txns_` 中删除。

## 6.4 UndoPhase

### 1. 设计

Redo 结束后，`active_txns_` 中剩下的事务表示宕机时还未提交也未回滚完成。Undo 阶段需要把这些事务全部回滚，保证恢复后的数据库只有已提交事务的结果。

### 2. 回滚逻辑

`UndoPhase` 遍历 `active_txns_`，对每个事务从最后一条日志开始沿 `prev_lsn_` 链向前回滚：

```cpp
while (lsn != INVALID_LSN) {
  auto iter = log_recs_.find(lsn);
  UndoLog(iter->second);
  lsn = iter->second->prev_lsn_;
}
```

`UndoLog` 根据日志类型执行反操作：

- Insert 的反操作：删除新插入的 key；
- Delete 的反操作：恢复旧 key/value；
- Update 的反操作：删除新 key/value，恢复旧 key/value；
- Begin/Commit/Abort 不直接修改数据。

完成后清空 `active_txns_`，表示所有未完成事务均已回滚。

## 6.5 测试结果

Recovery Manager 通过 `recovery_manager_test.cpp` 中的测试用例。该测试覆盖：

- 事务日志的 `prev_lsn_` 链是否正确；
- checkpoint 后 Redo 是否恢复日志中已发生的修改；
- Abort 事务是否在 Redo 阶段被回滚；
- 宕机时仍活跃的事务是否在 Undo 阶段被回滚。

最终恢复结果满足：已提交事务保留，已 abort 和未完成事务撤销。
