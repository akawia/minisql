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