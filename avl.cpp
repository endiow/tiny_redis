#include "avl.h"


static uint32_t avl_depth(AVLNode *node) {
    return node ? node->depth : 0;
}

static uint32_t avl_cnt(AVLNode *node) {
    return node ? node->cnt : 0;
}

static uint32_t max(uint32_t lhs, uint32_t rhs) {
    return lhs < rhs ? rhs : lhs;
}

// maintaining the depth and cnt field
/**
 * @brief 更新AVL树节点的深度和节点数量
 * 
 * 该函数用于更新AVL树节点的深度和节点数量。节点的深度是指从该节点到其最远叶子节点的最长路径上的节点数。节点的数量是指以该节点为根的子树中的节点总数。
 * 
 * @param node 指向AVL树节点的指针
 */
static void avl_update(AVLNode *node) {
    // 更新节点的深度为1加上左右子树深度的最大值
    node->depth = 1 + max(avl_depth(node->left), avl_depth(node->right));
    // 更新节点的数量为1加上左右子树节点数量的总和
    node->cnt = 1 + avl_cnt(node->left) + avl_cnt(node->right);
}


/**
 * @brief 将节点左旋
 * 
 * 该函数将给定的AVL树节点进行左旋操作。左旋操作会调整节点的子节点和父节点指针，以保持AVL树的平衡。
 * 
 * @param node 指向要左旋的AVL树节点的指针
 * @return AVLNode* 返回左旋后的新根节点指针
 */
static AVLNode *rot_left(AVLNode *node) {
    // 保存当前节点的右子节点
    AVLNode *new_node = node->right;
    // 如果新节点的左子节点存在，将其父节点指针指向当前节点
    if (new_node->left) {
        new_node->left->parent = node;
    }
    // 将当前节点的右子节点指针指向新节点的左子节点
    node->right = new_node->left;
    // 将新节点的左子节点指针指向当前节点
    new_node->left = node;
    // 将新节点的父节点指针指向当前节点的父节点
    new_node->parent = node->parent;
    // 将当前节点的父节点指针指向新节点
    node->parent = new_node;
    // 更新当前节点的深度和子节点数量
    avl_update(node);
    // 更新新节点的深度和子节点数量
    avl_update(new_node);
    // 返回左旋后的新根节点
    return new_node;
}


static AVLNode *rot_right(AVLNode *node) {
    AVLNode *new_node = node->left;
    if (new_node->right) {
        new_node->right->parent = node;
    }
    node->left = new_node->right;
    new_node->right = node;
    new_node->parent = node->parent;
    node->parent = new_node;
    avl_update(node);
    avl_update(new_node);
    return new_node;
}

// the left subtree is too deep
/**
 * @brief 修复左子树不平衡的情况
 * 
 * 该函数用于修复AVL树中左子树不平衡的情况。如果左子树的左子树深度小于左子树的右子树深度，
 * 则先对左子树进行左旋操作，然后再对根节点进行右旋操作，以恢复AVL树的平衡。
 * 
 * @param root 指向AVL树根节点的指针
 * @return AVLNode* 返回修复后的AVL树根节点指针
 */
static AVLNode *avl_fix_left(AVLNode *root) {
    // 如果左子树的左子树深度小于左子树的右子树深度
    if (avl_depth(root->left->left) < avl_depth(root->left->right)) {
        // 对左子树进行左旋操作
        root->left = rot_left(root->left);
    }
    // 对根节点进行右旋操作
    return rot_right(root);
}


// the right subtree is too deep
static AVLNode *avl_fix_right(AVLNode *root) {
    if (avl_depth(root->right->right) < avl_depth(root->right->left)) {
        root->right = rot_right(root->right);
    }
    return rot_left(root);
}

// fix imbalanced nodes and maintain invariants until the root is reached
/**
 * @brief 修复AVL树中的不平衡节点
 * 
 * 该函数通过旋转操作来修复AVL树中的不平衡节点，以保持树的平衡。
 * 
 * @param node 指向需要修复的AVL树节点的指针
 * @return AVLNode* 返回修复后的AVL树的根节点指针
 */
AVLNode *avl_fix(AVLNode *node) {
    // 循环直到节点平衡
    while (true) {
        // 更新当前节点的深度和节点数量
        avl_update(node);
        // 获取左子树的深度
        uint32_t l = avl_depth(node->left);
        // 获取右子树的深度
        uint32_t r = avl_depth(node->right);
        // 定义一个指针，用于指向父节点的子节点指针
        AVLNode **from = NULL;
        // 如果当前节点有父节点
        if (node->parent) {
            // 根据当前节点是父节点的左子节点还是右子节点，设置from指针
            from = (node->parent->left == node)
                ? &node->parent->left : &node->parent->right;
        }
        // 如果左子树的深度比右子树的深度大2
        if (l == r + 2) {
            // 修复左子树不平衡的情况
            node = avl_fix_left(node);
        // 如果右子树的深度比左子树的深度大2
        } else if (l + 2 == r) {
            // 修复右子树不平衡的情况
            node = avl_fix_right(node);
        }
        // 如果from指针为空，说明当前节点是根节点
        if (!from) {
            // 返回当前节点作为根节点
            return node;
        }
        // 将当前节点赋值给from指针指向的父节点的子节点指针
        *from = node;
        // 将当前节点更新为父节点
        node = node->parent;
    }
}


// detach a node and returns the new root of the tree
/**
 * @brief 从AVL树中删除一个节点
 * 
 * 该函数从AVL树中删除指定的节点，并通过旋转操作来修复可能出现的不平衡。
 * 
 * @param node 指向要删除的AVL树节点的指针
 * @return AVLNode* 返回删除操作后的AVL树的根节点指针
 */
AVLNode *avl_del(AVLNode *node) {
    if (node->right == NULL) {
        // 如果没有右子树，用左子树替换当前节点
        // 将左子树链接到父节点
        AVLNode *parent = node->parent;
        if (node->left) {
            node->left->parent = parent;
        }
        if (parent) {
            // 将左子树附加到父节点
            (parent->left == node ? parent->left : parent->right) = node->left;
            // 修复父节点以保持AVL树的平衡
            return avl_fix(parent);
        } else {
            // 如果删除的是根节点，返回左子树作为新的根节点
            return node->left;
        }
    } else {
        // 否则，找到当前节点的后继节点（即右子树中的最左节点）
        AVLNode *victim = node->right;
        while (victim->left) {
            victim = victim->left;
        }
        // 删除后继节点，并获取删除操作后的根节点
        AVLNode *root = avl_del(victim);

        // 用后继节点替换当前节点
        *victim = *node;
        if (victim->left) {
            victim->left->parent = victim;
        }
        if (victim->right) {
            victim->right->parent = victim;
        }
        AVLNode *parent = node->parent;
        if (parent) {
            // 将替换后的节点附加到父节点
            (parent->left == node ? parent->left : parent->right) = victim;
            return root;
        } else {
            // 如果删除的是根节点，返回替换后的节点作为新的根节点
            return victim;
        }
    }
}


// offset into the succeeding or preceding node.
// note: the worst-case is O(log(n)) regardless of how long the offset is.
/**
 * @brief 根据给定的偏移量在AVL树中查找节点
 * 
 * 该函数根据给定的偏移量在AVL树中查找节点。偏移量可以是正数或负数，表示相对于当前节点的位置。
 * 
 * @param node 指向AVL树中当前节点的指针
 * @param offset 相对于当前节点的偏移量
 * @return AVLNode* 返回偏移后的节点指针，如果偏移量超出范围则返回NULL
 */
AVLNode *avl_offset(AVLNode *node, int64_t offset) {
    int64_t pos = 0;    // 相对于起始节点的位置

    // 循环直到找到目标节点
    while (offset != pos) {
        // 如果目标节点在右子树中
        if (pos < offset && pos + avl_cnt(node->right) >= offset) {
            // 将当前节点移动到右子节点
            node = node->right;
            // 更新当前位置
            pos += avl_cnt(node->left) + 1;
        }
        // 如果目标节点在左子树中
        else if (pos > offset && pos - avl_cnt(node->left) <= offset) {
            // 将当前节点移动到左子节点
            node = node->left;
            // 更新当前位置
            pos -= avl_cnt(node->right) + 1;
        }
        // 如果目标节点不在当前子树中
        else {
            // 将当前节点移动到父节点
            AVLNode *parent = node->parent;
            // 如果没有父节点，说明已经到达根节点，返回NULL
            if (!parent) {
                return NULL;
            }
            // 根据当前节点是父节点的左子节点还是右子节点，更新当前位置
            if (parent->right == node) {
                pos -= avl_cnt(node->left) + 1;
            } else {
                pos += avl_cnt(node->right) + 1;
            }
            // 将当前节点更新为父节点
            node = parent;
        }
    }
    // 返回找到的节点
    return node;
}

