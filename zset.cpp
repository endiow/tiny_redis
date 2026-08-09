#include <assert.h>
#include <string.h>
#include <stdlib.h>
// proj
#include "zset.h"
#include "common.h"


/**
 * 创建一个新的 ZNode 结构体实例，并初始化其成员变量
 * @param name 要存储的字符串
 * @param len 字符串的长度
 * @param score 节点的分数
 * @return 指向新创建的 ZNode 结构体实例的指针
 */
static ZNode *znode_new(const char *name, size_t len, double score) {
    // 分配足够的内存来存储 ZNode 结构体和字符串数据
    ZNode *node = (ZNode *)malloc(sizeof(ZNode) + len);
    // 断言分配成功，实际项目中不建议这样做
    assert(node);   
    // 初始化 AVL 树节点
    avl_init(&node->tree);
    // 设置哈希表节点的下一个指针为 NULL
    node->hmap.next = NULL;
    // 计算并设置哈希表节点的哈希码
    node->hmap.hcode = str_hash((uint8_t *)name, len);
    // 设置节点的分数
    node->score = score;
    // 设置节点的字符串长度
    node->len = len;
    // 将字符串数据复制到节点的 name 成员中
    memcpy(&node->name[0], name, len);
    // 返回指向新创建的 ZNode 结构体实例的指针
    return node;
}


static uint32_t min(size_t lhs, size_t rhs) {
    return lhs < rhs ? lhs : rhs;
}

/**
 * 比较两个 ZNode 结构体实例的 score、name 和 len 成员，并返回比较结果
 * @param lhs 第一个 ZNode 结构体实例的 AVLNode 指针
 * @param score 第二个 ZNode 结构体实例的 score 值
 * @param name 第二个 ZNode 结构体实例的 name 指针
 * @param len 第二个 ZNode 结构体实例的 name 长度
 * @return 如果第一个 ZNode 结构体实例小于第二个 ZNode 结构体实例，则返回 true，否则返回 false
 */
static bool zless(
    AVLNode *lhs, double score, const char *name, size_t len)
{
    // 获取第一个 ZNode 结构体实例的指针
    ZNode *zl = container_of(lhs, ZNode, tree);
    // 如果两个 ZNode 结构体实例的 score 值不相等，则比较 score 值
    if (zl->score!= score) {
        return zl->score < score;
    }
    // 比较两个 ZNode 结构体实例的 name 成员，返回比较结果
    int rv = memcmp(zl->name, name, min(zl->len, len));
    if (rv!= 0) {
        return rv < 0;
    }
    // 如果 name 成员也相等，则比较 len 成员
    return zl->len < len;
}


/**
 * 比较两个 AVLNode 结构体实例的 score、name 和 len 成员，并返回比较结果
 * @param lhs 第一个 AVLNode 结构体实例
 * @param rhs 第二个 AVLNode 结构体实例
 * @return 如果第一个 AVLNode 结构体实例小于第二个 AVLNode 结构体实例，则返回 true，否则返回 false
 */
static bool zless(AVLNode *lhs, AVLNode *rhs) {
    // 获取第二个 AVLNode 结构体实例的 ZNode 结构体实例指针
    ZNode *zr = container_of(rhs, ZNode, tree);
    // 比较两个 ZNode 结构体实例的 score、name 和 len 成员，返回比较结果
    return zless(lhs, zr->score, zr->name, zr->len);
}


// insert into the AVL tree
static void tree_add(ZSet *zset, ZNode *node) {
    AVLNode *cur = NULL;            // 当前节点
    AVLNode **from = &zset->tree;   // 指向当前节点的下一个节点的指针
    while (*from) {                 // 树搜索
        cur = *from;
        // 根据比较结果决定下一个节点的方向
        from = zless(&node->tree, cur)? &cur->left : &cur->right;
    }
    // 将新节点附加到树中
    *from = &node->tree;            
    // 设置新节点的父节点
    node->tree.parent = cur;       
    // 修复 AVL 树以保持平衡
    zset->tree = avl_fix(&node->tree); 
}


/**
 * 更新 ZSet 中指定节点的分数
 * @param zset 指向 ZSet 结构体的指针
 * @param node 指向要更新的 ZNode 结构体的指针
 * @param score 新的分数值
 */
static void zset_update(ZSet *zset, ZNode *node, double score) {
    // 如果节点的分数没有变化，则直接返回
    if (node->score == score) {
        return;
    }
    // 从 AVL 树中删除指定节点
    zset->tree = avl_del(&node->tree);
    // 更新节点的分数
    node->score = score;
    // 重新初始化节点的 AVL 树
    avl_init(&node->tree);
    // 将更新后的节点重新插入到 AVL 树中
    tree_add(zset, node);
}


/**
 * 向 ZSet 中添加一个新的元素，或者更新一个已存在元素的分数
 * @param zset 指向 ZSet 结构体的指针
 * @param name 要添加或更新的元素的名称
 * @param len 名称的长度
 * @param score 要添加或更新的元素的分数
 * @return 如果元素是新添加的，返回 true，否则返回 false
 */
bool zset_add(ZSet *zset, const char *name, size_t len, double score) {
    // 查找是否已存在具有相同名称的元素
    ZNode *node = zset_lookup(zset, name, len);
    if (node) {
        // 如果元素已存在，则更新其分数
        zset_update(zset, node, score);
        return false;
    } else {
        // 如果元素不存在，则创建一个新的元素
        node = znode_new(name, len, score);
        // 将新元素插入到哈希表中
        hm_insert(&zset->hmap, &node->hmap);
        // 将新元素添加到 AVL 树中
        tree_add(zset, node);
        return true;
    }
}


// 辅助结构，用于哈希表查找
struct HKey {
    HNode node;
    const char *name = NULL;
    size_t len = 0;
};

/**
 * 比较两个 HNode 结构体实例的 name 和 len 成员，并返回比较结果
 * @param node 第一个 HNode 结构体实例
 * @param key 第二个 HNode 结构体实例
 * @return 如果两个 HNode 结构体实例的 name 和 len 成员相等，则返回 true，否则返回 false
 */
static bool hcmp(HNode *node, HNode *key) {
    // 获取第一个 HNode 结构体实例的 ZNode 结构体实例指针
    ZNode *znode = container_of(node, ZNode, hmap);
    // 获取第二个 HNode 结构体实例的 HKey 结构体实例指针
    HKey *hkey = container_of(key, HKey, node);
    // 如果两个 ZNode 结构体实例的 len 成员不相等，则返回 false
    if (znode->len!= hkey->len) {
        return false;
    }
    // 比较两个 ZNode 结构体实例的 name 成员，返回比较结果
    return 0 == memcmp(znode->name, hkey->name, znode->len);
}


/**
 * 在 ZSet 中查找具有指定名称的元素
 * @param zset 指向 ZSet 结构体的指针
 * @param name 要查找的元素的名称
 * @param len 名称的长度
 * @return 如果找到具有指定名称的元素，则返回指向该元素的 ZNode 结构体指针，否则返回 NULL
 */
ZNode *zset_lookup(ZSet *zset, const char *name, size_t len) {
    // 如果 ZSet 的 AVL 树为空，则直接返回 NULL
    if (!zset->tree) {
        return NULL;
    }

    HKey key;
    // 计算名称的哈希值
    key.node.hcode = str_hash((uint8_t *)name, len);
    // 设置名称和长度
    key.name = name;
    key.len = len;
    // 在哈希表中查找具有指定哈希值和名称的 HNode 结构体实例
    HNode *found = hm_lookup(&zset->hmap, &key.node, &hcmp);
    // 如果找到，则返回指向该元素的 ZNode 结构体指针，否则返回 NULL
    return found? container_of(found, ZNode, hmap) : NULL;
}


/**
 * 从 ZSet 中弹出具有指定名称的元素
 * @param zset 指向 ZSet 结构体的指针
 * @param name 要弹出的元素的名称
 * @param len 名称的长度
 * @return 如果找到具有指定名称的元素，则返回指向该元素的 ZNode 结构体指针，否则返回 NULL
 */
ZNode *zset_pop(ZSet *zset, const char *name, size_t len) {
    // 如果 ZSet 的 AVL 树为空，则直接返回 NULL
    if (!zset->tree) {
        return NULL;
    }

    HKey key;
    // 计算名称的哈希值
    key.node.hcode = str_hash((uint8_t *)name, len);
    // 设置名称和长度
    key.name = name;
    key.len = len;
    // 在哈希表中查找具有指定哈希值和名称的 HNode 结构体实例，并将其从哈希表中弹出
    HNode *found = hm_pop(&zset->hmap, &key.node, &hcmp);
    // 如果没有找到，则返回 NULL
    if (!found) {
        return NULL;
    }

    // 获取找到的 HNode 结构体实例对应的 ZNode 结构体实例指针
    ZNode *node = container_of(found, ZNode, hmap);
    // 从 AVL 树中删除该 ZNode 结构体实例对应的 AVLNode 结构体实例
    zset->tree = avl_del(&node->tree);
    // 返回弹出的 ZNode 结构体实例指针
    return node;
}


/**
 * 在 ZSet 中查找具有指定分数和名称的元素
 * @param zset 指向 ZSet 结构体的指针
 * @param score 要查找的元素的分数
 * @param name 要查找的元素的名称
 * @param len 名称的长度
 * @return 如果找到具有指定分数和名称的元素，则返回指向该元素的 ZNode 结构体指针，否则返回 NULL
 */
ZNode *zset_query(ZSet *zset, double score, const char *name, size_t len) {
    AVLNode *found = NULL;    // 找到的节点
    AVLNode *cur = zset->tree;    // 当前节点
    while (cur) {    // 遍历 AVL 树
        if (zless(cur, score, name, len)) {    // 如果当前节点的分数小于指定分数
            cur = cur->right;    // 则在右子树中继续查找
        } else {
            found = cur;    // 否则，将当前节点作为候选节点
            cur = cur->left;    // 并在左子树中继续查找
        }
    }
    return found? container_of(found, ZNode, tree) : NULL;    // 如果找到，则返回指向该元素的 ZNode 结构体指针，否则返回 NULL
}


/**
 * 在 ZNode 集合中查找相对于指定节点偏移量为 offset 的节点
 * @param node 起始节点
 * @param offset 偏移量
 * @return 如果找到偏移量为 offset 的节点，则返回该节点的指针，否则返回 NULL
 */
ZNode *znode_offset(ZNode *node, int64_t offset) {
    // 使用 avl_offset 函数在 AVL 树中查找偏移量为 offset 的节点
    AVLNode *tnode = node? avl_offset(&node->tree, offset) : NULL;
    // 如果找到节点，则返回该节点的指针，否则返回 NULL
    return tnode? container_of(tnode, ZNode, tree) : NULL;
}


void znode_del(ZNode *node) {
    free(node);
}

/**
 * 释放 AVL 树中的所有节点
 * @param node AVL 树的根节点
 */
static void tree_dispose(AVLNode *node) {
    // 如果节点为空，则直接返回
    if (!node) {
        return;
    }
    // 递归释放左子树中的所有节点
    tree_dispose(node->left);
    // 递归释放右子树中的所有节点
    tree_dispose(node->right);
    // 释放当前节点所对应的 ZNode 结构体实例，并将其从内存中删除
    znode_del(container_of(node, ZNode, tree));
}


// destroy the zset
void zset_dispose(ZSet *zset) {
    tree_dispose(zset->tree);
    hm_destroy(&zset->hmap);
}