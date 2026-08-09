#pragma once

#include <stdint.h>
#include <stddef.h>


#define container_of(ptr, type, member) ({                  \
    const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
    (type *)( (char *)__mptr - offsetof(type, member) );})


/**
 * @brief 计算字符串的哈希值
 * 
 * 该函数使用FNV-1a哈希算法计算给定字符串的哈希值。
 * 
 * @param data 指向字符串数据的指针
 * @param len 字符串的长度
 * @return uint64_t 计算得到的哈希值
 */
inline uint64_t str_hash(const uint8_t *data, size_t len) {
    // 初始化哈希值为FNV-1a的偏移量
    uint32_t h = 0x811C9DC5;
    // 遍历字符串中的每个字符
    for (size_t i = 0; i < len; i++) {
        // 将当前字符的ASCII值与哈希值相加，并乘以FNV-1a的质数
        h = (h + data[i]) * 0x01000193;
    }
    // 返回计算得到的哈希值
    return h;
}


enum {
    SER_NIL = 0,
    SER_ERR = 1,
    SER_STR = 2,
    SER_INT = 3,
    SER_DBL = 4,
    SER_ARR = 5,
};
