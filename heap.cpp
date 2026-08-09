#include <stddef.h>
#include <stdint.h>
#include "heap.h"


static size_t heap_parent(size_t i) {
    return (i + 1) / 2 - 1;
}

static size_t heap_left(size_t i) {
    return i * 2 + 1;
}

static size_t heap_right(size_t i) {
    return i * 2 + 2;
}

// 函数名: heap_up
// 参数: a - 堆数组
//       pos - 要调整的元素位置
// 功能: 向上调整堆中的元素，使其满足堆的性质
static void heap_up(HeapItem *a, size_t pos) {
    // 保存当前位置的元素
    HeapItem t = a[pos];
    // 当当前位置大于0且父节点的值大于当前元素时
    while (pos > 0 && a[heap_parent(pos)].val > t.val) {
        // 将父节点的值下移到当前位置
        a[pos] = a[heap_parent(pos)];
        // 更新父节点的引用
        *a[pos].ref = pos;
        // 更新当前位置为父节点的位置
        pos = heap_parent(pos);
    }
    // 将当前元素放到正确的位置
    a[pos] = t;
    // 更新当前元素的引用
    *a[pos].ref = pos;
}


// 函数名: heap_down
// 参数: a - 堆数组
//       pos - 要调整的元素位置
//       len - 堆的长度
// 功能: 向下调整堆中的元素，使其满足堆的性质
static void heap_down(HeapItem *a, size_t pos, size_t len) {
    // 保存当前位置的元素
    HeapItem t = a[pos];
    // 无限循环，直到找到合适的位置
    while (true) {
        // 找到当前节点的左子节点和右子节点
        size_t l = heap_left(pos);
        size_t r = heap_right(pos);
        // 初始化最小位置为-1，表示尚未找到最小的子节点
        size_t min_pos = -1;
        // 初始化最小的值为当前元素的值
        size_t min_val = t.val;
        // 如果左子节点存在且其值小于当前最小值
        if (l < len && a[l].val < min_val) {
            // 更新最小位置为左子节点的位置
            min_pos = l;
            // 更新最小的值为左子节点的值
            min_val = a[l].val;
        }
        // 如果右子节点存在且其值小于当前最小值
        if (r < len && a[r].val < min_val) {
            // 更新最小位置为右子节点的位置
            min_pos = r;
        }
        // 如果没有找到更小的子节点，即当前节点已经是最小的了
        if (min_pos == (size_t)-1) {
            // 跳出循环
            break;
        }
        // 将当前节点与最小的子节点交换
        a[pos] = a[min_pos];
        // 更新当前节点的引用
        *a[pos].ref = pos;
        // 更新当前位置为最小子节点的位置
        pos = min_pos;
    }
    // 将当前元素放到正确的位置
    a[pos] = t;
    // 更新当前元素的引用
    *a[pos].ref = pos;
}


// 函数名: heap_update
// 参数: a - 堆数组
//       pos - 要更新的元素位置
//       len - 堆的长度
// 功能: 更新堆中的元素，使其满足堆的性质
void heap_update(HeapItem *a, size_t pos, size_t len) {
    // 如果当前位置大于0且父节点的值大于当前元素的值
    if (pos > 0 && a[heap_parent(pos)].val > a[pos].val) {
        // 向上调整堆中的元素
        heap_up(a, pos);
    } 
    // 否则
    else {
        // 向下调整堆中的元素
        heap_down(a, pos, len);
    }
}
