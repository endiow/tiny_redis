#include <assert.h>
#include <stdlib.h>
#include "hashtable.h"


// n must be a power of 2
/**
 * @brief 初始化哈希表
 * 
 * 该函数用于初始化哈希表，分配内存并设置哈希表的初始状态。
 * 
 * @param htab 指向哈希表的指针
 * @param n 哈希表的大小，必须是2的幂
 */
static void h_init(HTab *htab, size_t n) {
    // 断言：确保哈希表大小n大于0且为2的幂
    assert(n > 0 && ((n - 1) & n) == 0);
    // 为哈希表分配内存，大小为n个HNode指针的大小
    htab->tab = (HNode **)calloc(sizeof(HNode *), n);
    // 设置哈希表的掩码，用于计算哈希值的索引
    htab->mask = n - 1;
    // 初始化哈希表的大小为0
    htab->size = 0;
}


// hashtable insertion
/**
 * @brief 向哈希表中插入节点
 * 
 * 该函数将给定的节点插入到哈希表中。如果哈希表中已经存在相同哈希值的节点，新节点将被插入到链表的头部。
 * 
 * @param htab 指向哈希表的指针
 * @param node 指向要插入的节点的指针
 */
static void h_insert(HTab *htab, HNode *node) {
    // 计算节点的哈希值在哈希表中的位置
    size_t pos = node->hcode & htab->mask;
    // 获取当前位置的链表头节点
    HNode *next = htab->tab[pos];
    // 将新节点的next指针指向当前链表头节点
    node->next = next;
    // 将新节点插入到链表头部
    htab->tab[pos] = node;
    // 增加哈希表的大小
    htab->size++;
}


// hashtable look up subroutine.
// Pay attention to the return value. It returns the address of
// the parent pointer that owns the target node,
// which can be used to delete the target node.
/**
 * @brief 在哈希表中查找节点
 * 
 * 该函数在哈希表中查找给定的节点。如果找到匹配的节点，则返回指向该节点的指针；否则返回NULL。
 * 
 * @param htab 指向哈希表的指针
 * @param key 指向要查找的节点的指针
 * @param eq 指向比较函数的指针，用于比较两个节点是否相等
 * @return HNode** 返回指向匹配节点的指针，如果未找到则返回NULL
 */
static HNode **h_lookup(HTab *htab, HNode *key, bool (*eq)(HNode *, HNode *)) {
    // 如果哈希表为空，直接返回NULL
    if (!htab->tab) {
        return NULL;
    }

    // 计算键的哈希值在哈希表中的位置
    size_t pos = key->hcode & htab->mask;
    // 获取当前位置的链表头节点的指针
    HNode **from = &htab->tab[pos];     // incoming pointer to the result
    // 遍历链表，查找匹配的节点
    for (HNode *cur; (cur = *from) != NULL; from = &cur->next) {
        // 如果当前节点的哈希值与键的哈希值相等，并且通过比较函数判断节点相等，则返回当前节点的指针
        if (cur->hcode == key->hcode && eq(cur, key)) {
            return from;
        }
    }
    // 如果未找到匹配的节点，返回NULL
    return NULL;
}


// remove a node from the chain
/**
 * @brief 从哈希表中分离（删除）一个节点
 * 
 * 该函数从哈希表中分离（删除）一个节点，并返回被分离的节点。
 * 
 * @param htab 指向哈希表的指针
 * @param from 指向要分离的节点的指针的指针
 * @return HNode* 返回被分离的节点的指针
 */
static HNode *h_detach(HTab *htab, HNode **from) {
    // 获取要分离的节点的指针
    HNode *node = *from;
    // 将当前节点的下一个节点的指针赋值给当前节点的指针
    *from = node->next;
    // 减少哈希表的大小
    htab->size--;
    // 返回被分离的节点的指针
    return node;
}


const size_t k_resizing_work = 128; // constant work

/**
 * @brief 帮助调整哈希表大小
 * 
 * 该函数在哈希表调整大小期间，从ht2中扫描节点并将它们移动到ht1中。
 * 
 * @param hmap 指向哈希表映射的指针
 */
static void hm_help_resizing(HMap *hmap) {
    size_t nwork = 0;
    // 当未完成足够的工作且ht2中仍有节点时，继续循环
    while (nwork < k_resizing_work && hmap->ht2.size > 0) {
        // 扫描ht2中的节点并将它们移动到ht1中
        HNode **from = &hmap->ht2.tab[hmap->resizing_pos];
        // 如果当前位置没有节点，则继续扫描下一个位置
        if (!*from) {
            hmap->resizing_pos++;
            continue;
        }

        // 将节点从ht2中分离并插入到ht1中
        h_insert(&hmap->ht1, h_detach(&hmap->ht2, from));
        nwork++;
    }

    // 如果ht2中没有节点且ht2表存在，则释放ht2表并重置ht2
    if (hmap->ht2.size == 0 && hmap->ht2.tab) {
        // 完成调整大小
        free(hmap->ht2.tab);
        hmap->ht2 = HTab{};
    }
}


/**
 * @brief 开始调整哈希表的大小
 * 
 * 该函数开始调整哈希表的大小，将当前哈希表替换为一个更大的哈希表，并初始化新的哈希表。
 * 
 * @param hmap 指向哈希表映射的指针
 */
static void hm_start_resizing(HMap *hmap) {
    // 断言：确保ht2表为空
    assert(hmap->ht2.tab == NULL);
    // 创建一个更大的哈希表并交换它们
    hmap->ht2 = hmap->ht1;
    // 初始化新的哈希表，大小为当前哈希表大小的两倍
    h_init(&hmap->ht1, (hmap->ht1.mask + 1) * 2);
    // 重置调整大小的位置
    hmap->resizing_pos = 0;
}


/**
 * @brief 在哈希表中查找节点
 * 
 * 该函数在哈希表中查找给定的节点。如果找到匹配的节点，则返回指向该节点的指针；否则返回NULL。
 * 
 * @param hmap 指向哈希表映射的指针
 * @param key 指向要查找的节点的指针
 * @param eq 指向比较函数的指针，用于比较两个节点是否相等
 * @return HNode* 返回指向匹配节点的指针，如果未找到则返回NULL
 */
HNode *hm_lookup(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *)) {
    // 调用帮助函数以确保哈希表在查找期间处于适当的状态
    hm_help_resizing(hmap);
    // 在第一个哈希表中查找节点
    HNode **from = h_lookup(&hmap->ht1, key, eq);
    // 如果在第一个哈希表中未找到节点，则在第二个哈希表中查找
    from = from ? from : h_lookup(&hmap->ht2, key, eq);
    // 返回找到的节点指针，如果未找到则返回NULL
    return from ? *from : NULL;
}


const size_t k_max_load_factor = 8;

/**
 * @brief 向哈希表中插入节点
 * 
 * 该函数将给定的节点插入到哈希表中。如果哈希表为空，则初始化哈希表。如果哈希表的负载因子超过阈值，则开始调整哈希表的大小。
 * 
 * @param hmap 指向哈希表映射的指针
 * @param node 指向要插入的节点的指针
 */
void hm_insert(HMap *hmap, HNode *node) {
    // 如果第一个哈希表为空，则初始化第一个哈希表，大小为4
    if (!hmap->ht1.tab) {
        h_init(&hmap->ht1, 4);
    }
    // 将节点插入到第一个哈希表中
    h_insert(&hmap->ht1, node);

    // 如果第二个哈希表为空，则检查是否需要调整哈希表的大小
    if (!hmap->ht2.tab) {
        // 计算第一个哈希表的负载因子
        size_t load_factor = hmap->ht1.size / (hmap->ht1.mask + 1);
        // 如果负载因子超过阈值，则开始调整哈希表的大小
        if (load_factor >= k_max_load_factor) {
            hm_start_resizing(hmap);
        }
    }
    // 帮助调整哈希表的大小
    hm_help_resizing(hmap);
}


/**
 * @brief 从哈希表中弹出（删除并返回）一个节点
 * 
 * 该函数从哈希表中弹出（删除并返回）一个节点。如果找到匹配的节点，则返回该节点的指针；否则返回NULL。
 * 
 * @param hmap 指向哈希表映射的指针
 * @param key 指向要弹出的节点的指针
 * @param eq 指向比较函数的指针，用于比较两个节点是否相等
 * @return HNode* 返回弹出的节点的指针，如果未找到则返回NULL
 */
HNode *hm_pop(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *)) {
    // 调用帮助函数以确保哈希表在查找期间处于适当的状态
    hm_help_resizing(hmap);
    // 在第一个哈希表中查找节点
    if (HNode **from = h_lookup(&hmap->ht1, key, eq)) {
        // 如果找到节点，则从第一个哈希表中分离并返回该节点
        return h_detach(&hmap->ht1, from);
    }
    // 在第二个哈希表中查找节点
    if (HNode **from = h_lookup(&hmap->ht2, key, eq)) {
        // 如果找到节点，则从第二个哈希表中分离并返回该节点
        return h_detach(&hmap->ht2, from);
    }
    // 如果未找到节点，则返回NULL
    return NULL;
}


size_t hm_size(HMap *hmap) {
    return hmap->ht1.size + hmap->ht2.size;
}

void hm_destroy(HMap *hmap) {
    free(hmap->ht1.tab);
    free(hmap->ht2.tab);
    *hmap = HMap{};
}
