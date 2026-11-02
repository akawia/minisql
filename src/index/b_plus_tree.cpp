#include "index/b_plus_tree.h"

#include <string>

#include "glog/logging.h"
#include "index/basic_comparator.h"
#include "index/generic_key.h"
#include "page/index_roots_page.h"

/**
 * TODO: Student Implement
 */
BPlusTree::BPlusTree(index_id_t index_id, BufferPoolManager *buffer_pool_manager, const KeyManager &KM,
                     int leaf_max_size, int internal_max_size)
    : index_id_(index_id),
      buffer_pool_manager_(buffer_pool_manager),
      processor_(KM),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size) {
        
  int key_size = processor_.GetKeySize();
  if (leaf_max_size_ == UNDEFINED_SIZE) {
    //叶子页 size = key + value(RowId)
    leaf_max_size_ = (PAGE_SIZE - LEAF_PAGE_HEADER_SIZE) / (key_size + sizeof(RowId)) - 1;
  }
  if (internal_max_size_ == UNDEFINED_SIZE) {
    //内部页 size = key + value(page_id)
    internal_max_size_ = (PAGE_SIZE - INTERNAL_PAGE_HEADER_SIZE) / (key_size + sizeof(page_id_t)) - 1;
  }

  //初始化根页id,从 index_roots_page 里读出
  auto *roots_page = reinterpret_cast<IndexRootsPage *>(buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID)->GetData());
  page_id_t root_id = INVALID_PAGE_ID;
  if (roots_page->GetRootId(index_id_, &root_id)) {
    root_page_id_ = root_id;
  }
  buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, false);
}

/// destroy the b plus tree
void BPlusTree::Destroy(page_id_t current_page_id) {
  //不带参数就是销毁整棵树的顶层调用
  bool is_root_call = (current_page_id == INVALID_PAGE_ID);
  if (is_root_call) {
    current_page_id = root_page_id_;
  }
  //空树直接返回
  if (current_page_id == INVALID_PAGE_ID) {
    return;
  }

  //fetch page
  Page *page = buffer_pool_manager_->FetchPage(current_page_id);
  auto *node = reinterpret_cast<BPlusTreePage *>(page->GetData());

  //中间页：先递归删光所有孩子
  if (!node->IsLeafPage()) {
    auto *internal_node = reinterpret_cast<InternalPage *>(node);
    for (int i = 0; i < internal_node->GetSize(); i++) {
      page_id_t child_page_id = internal_node->ValueAt(i);
      Destroy(child_page_id);
    }
  }
  //叶子页：直接删掉当前页
  //删掉当前页
  buffer_pool_manager_->UnpinPage(current_page_id, false);
  buffer_pool_manager_->DeletePage(current_page_id);

  //顶层调用 把rootid从 index_roots_page 里删掉
  if (is_root_call) {
    root_page_id_ = INVALID_PAGE_ID;
    auto *roots_page = reinterpret_cast<IndexRootsPage *>(buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID)->GetData());
    roots_page->Delete(index_id_);
    buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, true);
  }
}

/*
 * Helper function to decide whether current b+tree is empty
 */
bool BPlusTree::IsEmpty() const {
  //没有根页就是空树
  return root_page_id_ == INVALID_PAGE_ID;
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
bool BPlusTree::GetValue(const GenericKey *key, std::vector<RowId> &result, Txn *transaction) {
  if (IsEmpty()) {
    return false;
  }
  //尝试find叶子页,如果找不到就返回false
  Page *leaf_page = FindLeafPage(key, INVALID_PAGE_ID, false);
  if (leaf_page == nullptr) {
    return false;
  }
  //查找key
  auto *leaf = reinterpret_cast<LeafPage *>(leaf_page->GetData());
  RowId value;
  bool found = leaf->Lookup(key, value, processor_);
  if (found) {
    result.push_back(value);
  }

  //用完一定要 unpin
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
  return found;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::Insert(GenericKey *key, const RowId &value, Txn *transaction) {
  //empty
  if(IsEmpty()) {
    StartNewTree(key, value);
    return true;
  }

  return InsertIntoLeaf(key, value, transaction);
}
/*
 * Insert constant key & value pair into an empty tree
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then update b+
 * tree's root page id and insert entry directly into leaf page.
 */
void BPlusTree::StartNewTree(GenericKey *key, const RowId &value) {
  //ask for new page
  page_id_t new_page_id;
  Page *new_page = buffer_pool_manager_->NewPage(new_page_id);
  if (new_page == nullptr) {
    throw std::bad_alloc();
  }

  auto *leaf = reinterpret_cast<LeafPage *>(new_page->GetData());
  //必须先 Init:它是根叶页,parent 传 INVALID,key_size/max_size 来自成员
  leaf->Init(new_page_id, INVALID_PAGE_ID, processor_.GetKeySize(), leaf_max_size_);

  //更新并持久化 root(第一次建树用 insert_record=1,往 roots 页插入记录)
  root_page_id_ = new_page_id;
  UpdateRootPageId(1);

  //插入第一个键值对
  leaf->Insert(key, value, processor_);

  //用完 unpin(改了内容,dirty)
  buffer_pool_manager_->UnpinPage(new_page_id, true);
}

/*
 * Insert constant key & value pair into leaf page
 * User needs to first find the right leaf page as insertion target, then look
 * through leaf page to see whether insert key exist or not. If exist, return
 * immediately, otherwise insert entry. Remember to deal with split if necessary.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::InsertIntoLeaf(GenericKey *key, const RowId &value, Txn *transaction) {
  //find right leaf page
  Page *leaf_page = FindLeafPage(key, INVALID_PAGE_ID, false);
  if (leaf_page == nullptr) {
    return false;
  }
  auto *leaf = reinterpret_cast<LeafPage *>(leaf_page->GetData());

  //exist and return false
  RowId tmp;
  if (leaf->Lookup(key, tmp, processor_)) {
    buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(), false);
    return false;
  }

  //otherwise insert entry
  leaf->Insert(key, value, processor_);

  if (leaf->GetSize() <= leaf->GetMaxSize()) {
    buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(), true);
    return true;
  }

  //Remember to deal with split if necessary
  LeafPage *new_leaf = Split(leaf, transaction);
  InsertIntoParent(leaf, new_leaf->KeyAt(0), new_leaf, transaction);

  //新旧都要unpin
  buffer_pool_manager_->UnpinPage(new_leaf->GetPageId(), true);
  buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(), true);
  return true;
}

/*
 * Split input page and return newly created page.
 * Using template N to represent either internal page or leaf page.
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then move half
 * of key & value pairs from input page to newly created page
 */
BPlusTreeInternalPage *BPlusTree::Split(InternalPage *node, Txn *transaction) {
  //ask for new page
  page_id_t new_page_id;
  Page *new_page = buffer_pool_manager_->NewPage(new_page_id);
  if (new_page == nullptr) {
    throw std::bad_alloc();
  }

  //then move half of key & value pairs from input page to newly created page
  auto *new_node = reinterpret_cast<InternalPage *>(new_page->GetData());
  new_node->Init(new_page_id, node->GetParentPageId(), processor_.GetKeySize(), internal_max_size_);
  
  node->MoveHalfTo(new_node, buffer_pool_manager_);

  return new_node;
}

BPlusTreeLeafPage *BPlusTree::Split(LeafPage *node, Txn *transaction) {
  //ask for new page
  page_id_t new_page_id;
  Page *new_page = buffer_pool_manager_->NewPage(new_page_id);
  if (new_page == nullptr) {
    throw std::bad_alloc();
  }

  //then move half of key & value pairs from input page to newly created page
  auto *new_node = reinterpret_cast<LeafPage *>(new_page->GetData());
  new_node->Init(new_page_id, node->GetParentPageId(), processor_.GetKeySize(), leaf_max_size_);
  
  node->MoveHalfTo(new_node);

  //接上叶子链表
  new_node->SetNextPageId(node->GetNextPageId());
  node->SetNextPageId(new_page_id);

  return new_node;

}

/*
 * Insert key & value pair into internal page after split
 * @param   old_node      input page from split() method
 * @param   key
 * @param   new_node      returned page from split() method
 * User needs to first find the parent page of old_node, parent node must be
 * adjusted to take info of new_node into account. Remember to deal with split
 * recursively if necessary.
 */
void BPlusTree::InsertIntoParent(BPlusTreePage *old_node, GenericKey *key, BPlusTreePage *new_node, Txn *transaction) {
  //find ola_node's parent page
  //如果 old_node 是根页,就新建一个根页,把 old_node 和 new_node 都接上去
  if(old_node->IsRootPage()){
    page_id_t new_root_id;
    Page *new_root_page = buffer_pool_manager_->NewPage(new_root_id);
    if (new_root_page == nullptr) {
      throw std::bad_alloc();
    }

    auto *new_root = reinterpret_cast<InternalPage *>(new_root_page->GetData());
    new_root->Init(new_root_id, INVALID_PAGE_ID, processor_.GetKeySize(), internal_max_size_);
    new_root->PopulateNewRoot(old_node->GetPageId(), key, new_node->GetPageId());

    //更新 old_node 和 new_node 的 parent
    old_node->SetParentPageId(new_root_id);
    new_node->SetParentPageId(new_root_id);

    root_page_id_ = new_root_id;
    UpdateRootPageId(0);
    buffer_pool_manager_->UnpinPage(new_root_id, true);
    return;
  }

  //把key插入到 old_node 的 parent
  page_id_t parent_id = old_node->GetParentPageId();
  Page *parent_page = buffer_pool_manager_->FetchPage(parent_id);
  auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());

  //插入 key 到 parent
  new_node->SetParentPageId(parent_id);
  parent->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());
  
  //如果 parent 超容量,就 split parent 并递归插入
  if(parent->GetSize() > internal_max_size_) {
    InternalPage *new_parent = Split(parent, transaction);
    InsertIntoParent(parent, new_parent->KeyAt(0), new_parent, transaction);
    buffer_pool_manager_->UnpinPage(new_parent->GetPageId(), true);
  }
  buffer_pool_manager_->UnpinPage(parent_id, true);
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */
void BPlusTree::Remove(const GenericKey *key, Txn *transaction) {
  //empty
  if (IsEmpty()) {
    return;
  }

  //find right leaf page
  Page *leaf_page = FindLeafPage(key, INVALID_PAGE_ID, false);
  if (leaf_page == nullptr) {
    return;
  }

  auto *leaf = reinterpret_cast<LeafPage *>(leaf_page->GetData());
  //删除 key 
  int size_before = leaf->GetSize();
  int size_after = leaf->RemoveAndDeleteRecord(key, processor_);

  //如果size没有变化,说明没找到key
  if(size_after == size_before) {
    buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(), false);
    return;
  }

  //可能溢出
  bool success = CoalesceOrRedistribute(leaf, transaction);

  page_id_t leaf_page_id = leaf->GetPageId();
  buffer_pool_manager_->UnpinPage(leaf_page_id, true);

  if(success) {
    //如果叶页被删除,就 unpin
    buffer_pool_manager_->UnpinPage(leaf_page_id, false);
  }

}

/* todo
 * User needs to first find the sibling of input page. If sibling's size + input
 * page's size > page's max size, then redistribute. Otherwise, merge.
 * Using template N to represent either internal page or leaf page.
 * @return: true means target leaf page should be deleted, false means no
 * deletion happens
 */
template <typename N>
bool BPlusTree::CoalesceOrRedistribute(N *&node, Txn *transaction) {
  //如果是根页,就调整根页
  if(node->IsRootPage()) {
    return AdjustRoot(node);
  }

  if(node->GetSize() >= node->GetMinSize()) {
    return false;
  }

  //下溢:取父、定位 node、选兄弟(默认取左兄弟,node 最左时才取右兄弟)
  page_id_t parent_id = node->GetParentPageId();
  auto *parent = reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(parent_id)->GetData());
  int index = parent->ValueIndex(node->GetPageId());
  int sibling_index = (index == 0) ? 1 : index - 1;
  page_id_t sibling_page_id = parent->ValueAt(sibling_index);
  auto *sibling = reinterpret_cast<N *>(buffer_pool_manager_->FetchPage(sibling_page_id)->GetData());

  //兄弟+自己塞不进一页 → 借一个(Redistribute);index 传 node 在父中的下标(决定借的方向)
  if (sibling->GetSize() + node->GetSize() > node->GetMaxSize()) {
    Redistribute(sibling, node, index);
    buffer_pool_manager_->UnpinPage(sibling_page_id, true);
    buffer_pool_manager_->UnpinPage(parent_id, true);
    return false;
  }

  //塞得下 → 合并,统一"右页并入左页",由此决定谁被清空、谁来删它
  bool node_should_delete;
  bool parent_should_delete;
  if (index == 0) {
    //node 最左,sibling 在右:sibling 并入 node,被清空的是 sibling(本函数 pin,本函数删)
    parent_should_delete = Coalesce(node, sibling, parent, 1, transaction);
    buffer_pool_manager_->UnpinPage(sibling_page_id, true);
    buffer_pool_manager_->DeletePage(sibling_page_id);
    node_should_delete = false;  //node 存活
  } else {
    //sibling 在左,node 在右:node 并入 sibling,被清空的是 node(交给调用方删)
    parent_should_delete = Coalesce(sibling, node, parent, index, transaction);
    buffer_pool_manager_->UnpinPage(sibling_page_id, true);
    node_should_delete = true;
  }

  //父页:合并使父少一项,递归可能判定父也该删
  if (parent_should_delete) {
    buffer_pool_manager_->UnpinPage(parent_id, true);
    buffer_pool_manager_->DeletePage(parent_id);
  } else {
    buffer_pool_manager_->UnpinPage(parent_id, true);
  }
  return node_should_delete;
}


/*
 * Move all the key & value pairs from one page to its sibling page, and notify
 * buffer pool manager to delete this page. Parent page must be adjusted to
 * take info of deletion into account. Remember to deal with coalesce or
 * redistribute recursively if necessary.
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 * @param   parent             parent page of input "node"
 * @return  true means parent node should be deleted, false means no deletion happened
 */
bool BPlusTree::Coalesce(LeafPage *&neighbor_node, LeafPage *&node, InternalPage *&parent, int index,
                         Txn *transaction) {
  //node在左 neighbor在右
  node->MoveAllTo(neighbor_node);
  parent->Remove(index);
  return CoalesceOrRedistribute(parent, transaction);
}

bool BPlusTree::Coalesce(InternalPage *&neighbor_node, InternalPage *&node, InternalPage *&parent, int index,
                         Txn *transaction) {
  //中间页合并
  GenericKey *key = parent->KeyAt(index);
  node->MoveAllTo(neighbor_node, key, buffer_pool_manager_);
  parent->Remove(index);
  return CoalesceOrRedistribute(parent, transaction);
}

/*
 * Redistribute key & value pairs from one page to its sibling page. If index ==
 * 0, move sibling page's first key & value pair into end of input "node",
 * otherwise move sibling page's last key & value pair into head of input
 * "node".
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 */
void BPlusTree::Redistribute(LeafPage *neighbor_node, LeafPage *node, int index) {
  page_id_t parent_id = node->GetParentPageId();
  auto *parent = reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(parent_id)->GetData());

  if (index == 0) {
    //node 最左,neighbor 是右兄弟(父中下标 1):借 neighbor 第一个到 node 末尾
    neighbor_node->MoveFirstToEndOf(node);
    //neighbor 失去首项,它的新首键就是新分界 → 更新父第 1 项
    parent->SetKeyAt(1, neighbor_node->KeyAt(0));
  } else {
    //neighbor 是左兄弟,node 在右:借 neighbor 最后一个到 node 头部
    neighbor_node->MoveLastToFrontOf(node);
    //node 多了新首项,它就是新分界 → 更新父第 index 项
    parent->SetKeyAt(index, node->KeyAt(0));
  }
  buffer_pool_manager_->UnpinPage(parent_id, true);
}

void BPlusTree::Redistribute(InternalPage *neighbor_node, InternalPage *node, int index) {
  page_id_t parent_id = node->GetParentPageId();
  auto *parent = reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(parent_id)->GetData());

  if (index == 0) {
    //node 最左,neighbor 右兄弟(父下标 1):父第 1 项下放,借 neighbor 第一个孩子到 node 末尾
    neighbor_node->MoveFirstToEndOf(node, parent->KeyAt(1), buffer_pool_manager_);
    //MoveFirstToEndOf 内部 Remove(0) 把 neighbor 原 K1 挪到了槽 0 → 它升上去当新分界
    parent->SetKeyAt(1, neighbor_node->KeyAt(0));
  } else {
    //neighbor 左兄弟,node 在右:neighbor 最后一个 key 要升上去当新分界,搬动【之前】先抓住
    GenericKey *up_key = neighbor_node->KeyAt(neighbor_node->GetSize() - 1);
    //父第 index 项下放,借 neighbor 最后一个孩子到 node 头部
    neighbor_node->MoveLastToFrontOf(node, parent->KeyAt(index), buffer_pool_manager_);
    parent->SetKeyAt(index, up_key);
  }
  buffer_pool_manager_->UnpinPage(parent_id, true);
}
/*
 * Update root page if necessary
 * NOTE: size of root page can be less than min size and this method is only
 * called within coalesceOrRedistribute() method
 * case 1: when you delete the last element in root page, but root page still
 * has one last child
 * case 2: when you delete the last element in whole b+ tree
 * @return : true means root page should be deleted, false means no deletion
 * happened
 */
bool BPlusTree::AdjustRoot(BPlusTreePage *old_root_node) {
  //case 1:根是中间页,删到只剩一个孩子(size==1)→ 让那个唯一孩子成为新根
  if (!old_root_node->IsLeafPage() && old_root_node->GetSize() == 1) {
    auto *old_root = reinterpret_cast<InternalPage *>(old_root_node);
    page_id_t child_id = old_root->RemoveAndReturnOnlyChild();
    //唯一孩子升为根,它的父变成 INVALID
    auto *child = reinterpret_cast<BPlusTreePage *>(buffer_pool_manager_->FetchPage(child_id)->GetData());
    child->SetParentPageId(INVALID_PAGE_ID);
    buffer_pool_manager_->UnpinPage(child_id, true);
    //更新并持久化新 root
    root_page_id_ = child_id;
    UpdateRootPageId(0);
    return true;  //旧根应被删
  }

  //case 2:根是叶页,删到空了(size==0)→ 整棵树空
  if (old_root_node->IsLeafPage() && old_root_node->GetSize() == 0) {
    root_page_id_ = INVALID_PAGE_ID;
    UpdateRootPageId(0);
    return true;  //旧根应被删
  }

  //其余:根还正常,不动
  return false;
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/*
 * Input parameter is void, find the left most leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin() {
  //空树就直接是 end
  if (IsEmpty()) {
    return End();
  }
  //leftMost=true 一路取最左孩子,落到全树最小 key 的叶页
  Page *leaf_page = FindLeafPage(nullptr, INVALID_PAGE_ID, true);
  page_id_t leaf_id = leaf_page->GetPageId();
  //FindLeafPage 把叶页 pin 了一次;IndexIterator 构造时还会再 Fetch 一次,这里先 unpin 抵消
  buffer_pool_manager_->UnpinPage(leaf_id, false);
  return IndexIterator(leaf_id, buffer_pool_manager_, 0);
}

/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin(const GenericKey *key) {
  //空树就直接是 end
  if (IsEmpty()) {
    return End();
  }
  //找到 key 应该所在的叶页
  Page *leaf_page = FindLeafPage(key, INVALID_PAGE_ID, false);
  auto *leaf = reinterpret_cast<LeafPage *>(leaf_page->GetData());
  //KeyIndex 返回第一个 >= key 的下标,作为迭代起点
  int index = leaf->KeyIndex(key, processor_);
  page_id_t leaf_id = leaf->GetPageId();
  //同样先 unpin 抵消 FindLeafPage 的 pin,交给 IndexIterator 持有
  buffer_pool_manager_->UnpinPage(leaf_id, false);
  return IndexIterator(leaf_id, buffer_pool_manager_, index);
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
IndexIterator BPlusTree::End() {
  return IndexIterator();
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/
/*
 * Find leaf page containing particular key, if leftMost flag == true, find
 * the left most leaf page
 * Note: the leaf page is pinned, you need to unpin it after use.
 */
Page *BPlusTree::FindLeafPage(const GenericKey *key, page_id_t page_id, bool leftMost) {
  if (IsEmpty()) {
    return nullptr;
  }
  //未指定起点就从根开始
  page_id_t cur_id = (page_id == INVALID_PAGE_ID) ? root_page_id_ : page_id;
  auto *page = buffer_pool_manager_->FetchPage(cur_id);
  auto *node = reinterpret_cast<BPlusTreePage *>(page->GetData());

  //一直往下走,直到落到叶页
  while (!node->IsLeafPage()) {
    auto *internal = reinterpret_cast<InternalPage *>(node);
    //leftMost 时一直取最左孩子(给迭代器 Begin() 用);否则按 key 导航
    page_id_t child_id = leftMost ? internal->ValueAt(0) : internal->Lookup(key, processor_);
    //下降一层前先把当前页 unpin
    buffer_pool_manager_->UnpinPage(cur_id, false);
    cur_id = child_id;
    page = buffer_pool_manager_->FetchPage(cur_id);
    node = reinterpret_cast<BPlusTreePage *>(page->GetData());
  }
  //返回时叶页仍处于 pinned 状态,由调用方负责 unpin
  return page;
}

/*
 * Update/Insert root page id in header page(where page_id = INDEX_ROOTS_PAGE_ID,
 * header_page isdefined under include/page/header_page.h)
 * Call this method everytime root page id is changed.
 * @parameter: insert_record      default value is false. When set to true,
 * insert a record <index_name, current_page_id> into header page instead of
 * updating it.
 */
void BPlusTree::UpdateRootPageId(int insert_record) {
  //root page 定义在 INDEX_ROOTS_PAGE_ID(=1),不是 0(0 是 catalog meta)
  auto *roots_page = reinterpret_cast<IndexRootsPage *>(buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID)->GetData());
  if (insert_record != 0) {
    //第一次建树:插入一条 <index_id, root_page_id> 记录
    roots_page->Insert(index_id_, root_page_id_);
  } else {
    //根换了:更新已有记录
    roots_page->Update(index_id_, root_page_id_);
  }
  buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, true);
}

/**
 * This method is used for debug only, You don't need to modify
 */
void BPlusTree::ToGraph(BPlusTreePage *page, BufferPoolManager *bpm, std::ofstream &out, Schema *schema) const {
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    // Print node name
    out << leaf_prefix << leaf->GetPageId();
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">P=" << leaf->GetPageId()
        << ",Parent=" << leaf->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">"
        << "max_size=" << leaf->GetMaxSize() << ",min_size=" << leaf->GetMinSize() << ",size=" << leaf->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf->GetSize(); i++) {
      Row ans;
      processor_.DeserializeToKey(leaf->KeyAt(i), ans, schema);
      out << "<TD>" << ans.GetField(0)->toString() << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf->GetNextPageId() != INVALID_PAGE_ID) {
      out << leaf_prefix << leaf->GetPageId() << " -> " << leaf_prefix << leaf->GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << leaf->GetPageId() << " " << leaf_prefix << leaf->GetNextPageId() << "};\n";
    }

    // Print parent links if there is a parent
    if (leaf->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << leaf->GetParentPageId() << ":p" << leaf->GetPageId() << " -> " << leaf_prefix
          << leaf->GetPageId() << ";\n";
    }
  } else {
    auto *inner = reinterpret_cast<InternalPage *>(page);
    // Print node name
    out << internal_prefix << inner->GetPageId();
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">P=" << inner->GetPageId()
        << ",Parent=" << inner->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">"
        << "max_size=" << inner->GetMaxSize() << ",min_size=" << inner->GetMinSize() << ",size=" << inner->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner->GetSize(); i++) {
      out << "<TD PORT=\"p" << inner->ValueAt(i) << "\">";
      if (i > 0) {
        Row ans;
        processor_.DeserializeToKey(inner->KeyAt(i), ans, schema);
        out << ans.GetField(0)->toString();
      } else {
        out << " ";
      }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Parent link
    if (inner->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << inner->GetParentPageId() << ":p" << inner->GetPageId() << " -> " << internal_prefix
          << inner->GetPageId() << ";\n";
    }
    // Print leaves
    for (int i = 0; i < inner->GetSize(); i++) {
      auto child_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i))->GetData());
      ToGraph(child_page, bpm, out, schema);
      if (i > 0) {
        auto sibling_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i - 1))->GetData());
        if (!sibling_page->IsLeafPage() && !child_page->IsLeafPage()) {
          out << "{rank=same " << internal_prefix << sibling_page->GetPageId() << " " << internal_prefix
              << child_page->GetPageId() << "};\n";
        }
        bpm->UnpinPage(sibling_page->GetPageId(), false);
      }
    }
  }
  bpm->UnpinPage(page->GetPageId(), false);
}

/**
 * This function is for debug only, you don't need to modify
 */
void BPlusTree::ToString(BPlusTreePage *page, BufferPoolManager *bpm) const {
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    std::cout << "Leaf Page: " << leaf->GetPageId() << " parent: " << leaf->GetParentPageId()
              << " next: " << leaf->GetNextPageId() << std::endl;
    for (int i = 0; i < leaf->GetSize(); i++) {
      std::cout << leaf->KeyAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
  } else {
    auto *internal = reinterpret_cast<InternalPage *>(page);
    std::cout << "Internal Page: " << internal->GetPageId() << " parent: " << internal->GetParentPageId() << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      std::cout << internal->KeyAt(i) << ": " << internal->ValueAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      ToString(reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(internal->ValueAt(i))->GetData()), bpm);
      bpm->UnpinPage(internal->ValueAt(i), false);
    }
  }
}

bool BPlusTree::Check() {
  bool all_unpinned = buffer_pool_manager_->CheckAllUnpinned();
  if (!all_unpinned) {
    LOG(ERROR) << "problem in page unpin" << endl;
  }
  return all_unpinned;
}