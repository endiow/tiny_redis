#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <string>
#include <vector>
// proj
#include "hashtable.h"
#include "zset.h"
#include "list.h"
#include "heap.h"
#include "thread_pool.h"
#include "common.h"


static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static uint64_t get_monotonic_usec() {
    timespec tv = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return uint64_t(tv.tv_sec) * 1000000 + tv.tv_nsec / 1000;
}

/**
 * @brief 将文件描述符设置为非阻塞模式
 * 
 * 该函数将给定的文件描述符设置为非阻塞模式。如果设置失败，则打印错误信息并终止程序。
 * 
 * @param fd 要设置为非阻塞模式的文件描述符
 */
static void fd_set_nb(int fd) {
    // 清除 errno 变量，以便检查后续系统调用的错误
    errno = 0;
    // 获取文件描述符的当前标志
    int flags = fcntl(fd, F_GETFL, 0);
    // 如果获取标志失败，则打印错误信息并终止程序
    if (errno) {
        die("fcntl error");
        return;
    }

    // 将 O_NONBLOCK 标志添加到文件描述符的当前标志中
    flags |= O_NONBLOCK;

    // 清除 errno 变量，以便检查后续系统调用的错误
    errno = 0;
    // 设置文件描述符的新标志
    (void)fcntl(fd, F_SETFL, flags);
    // 如果设置标志失败，则打印错误信息并终止程序
    if (errno) {
        die("fcntl error");
    }
}


struct Conn;

// global variables
static struct {
    HMap db;
    // a map of all client connections, keyed by fd
    std::vector<Conn *> fd2conn;
    // timers for idle connections
    DList idle_list;
    // timers for TTLs
    std::vector<HeapItem> heap;
    // the thread pool
    TheadPool tp;
} g_data;

const size_t k_max_msg = 4096;

enum {
    STATE_REQ = 0,
    STATE_RES = 1,
    STATE_END = 2,  // mark the connection for deletion
};

struct Conn {
    int fd = -1;
    uint32_t state = 0;     // either STATE_REQ or STATE_RES
    // buffer for reading
    size_t rbuf_size = 0;
    uint8_t rbuf[4 + k_max_msg];
    // buffer for writing
    size_t wbuf_size = 0;
    size_t wbuf_sent = 0;
    uint8_t wbuf[4 + k_max_msg];
    uint64_t idle_start = 0;
    // timer
    DList idle_list;
};

/**
 * @brief 将连接放入连接数组中
 * 
 * 该函数将给定的连接放入连接数组中。如果连接数组的大小不足以容纳该连接，则会调整数组的大小。
 * 
 * @param fd2conn 连接数组
 * @param conn 要放入的连接
 */
static void conn_put(std::vector<Conn *> &fd2conn, struct Conn *conn) {
    // 如果连接数组的大小小于或等于连接的文件描述符，则调整数组的大小
    if (fd2conn.size() <= (size_t)conn->fd) {
        fd2conn.resize(conn->fd + 1);
    }
    // 将连接放入数组中
    fd2conn[conn->fd] = conn;
}


/**
 * @brief 接受新的连接并进行初始化
 * 
 * 该函数用于接受一个新的客户端连接，并对其进行初始化。它会将新连接的文件描述符设置为非阻塞模式，
 * 创建一个 Conn 结构体来管理这个连接，设置连接的初始状态，记录连接的空闲开始时间，
 * 将连接插入到空闲列表中，并将连接放入全局的连接数组中。
 * 
 * @param fd 监听套接字的文件描述符
 * @return int32_t 返回0表示成功，返回-1表示失败
 */
static int32_t accept_new_conn(int fd) {
    // accept
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    // 接受新的客户端连接
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
    if (connfd < 0) {
        msg("accept() error");
        return -1;  // error
    }

    // set the new connection fd to nonblocking mode
    // 将新连接的文件描述符设置为非阻塞模式
    fd_set_nb(connfd);
    // creating the struct Conn
    // 创建一个 Conn 结构体来管理这个连接
    struct Conn *conn = (struct Conn *)malloc(sizeof(struct Conn));
    if (!conn) {
        close(connfd);
        return -1;
    }
    // 设置连接的文件描述符
    conn->fd = connfd;
    // 设置连接的初始状态为请求状态
    conn->state = STATE_REQ;
    // 初始化读缓冲区大小
    conn->rbuf_size = 0;
    // 初始化写缓冲区大小
    conn->wbuf_size = 0;
    // 初始化已发送的写缓冲区大小
    conn->wbuf_sent = 0;
    // 记录连接的空闲开始时间
    conn->idle_start = get_monotonic_usec();
    // 将连接插入到空闲列表中
    dlist_insert_before(&g_data.idle_list, &conn->idle_list);
    // 将连接放入全局的连接数组中
    conn_put(g_data.fd2conn, conn);
    return 0;
}


static void state_req(Conn *conn);
static void state_res(Conn *conn);

const size_t k_max_args = 1024;

/**
 * @brief 解析请求数据
 * 
 * 该函数用于解析从客户端接收到的请求数据。它会将请求数据解析为一个字符串向量，其中每个字符串代表一个参数。
 * 
 * @param data 指向请求数据的指针
 * @param len 请求数据的长度
 * @param out 用于存储解析结果的字符串向量
 * @return int32_t 返回0表示解析成功，返回-1表示解析失败
 */
static int32_t parse_req(
    const uint8_t *data, size_t len, std::vector<std::string> &out)
{
    // 检查请求数据长度是否小于4字节，如果是，则返回-1表示解析失败
    if (len < 4) {
        return -1;
    }

    uint32_t n = 0;
    // 从请求数据的前4个字节中解析出参数的数量
    memcpy(&n, &data[0], 4);
    // 检查参数数量是否超过最大允许的参数数量，如果是，则返回-1表示解析失败
    if (n > k_max_args) {
        return -1;
    }

    size_t pos = 4;
    // 循环解析每个参数
    while (n--) {
        // 检查当前位置加上4字节是否超过请求数据的长度，如果是，则返回-1表示解析失败
        if (pos + 4 > len) {
            return -1;
        }
        uint32_t sz = 0;
        // 从当前位置开始的4个字节中解析出参数的长度
        memcpy(&sz, &data[pos], 4);
        // 检查当前位置加上4字节再加上参数的长度是否超过请求数据的长度，如果是，则返回-1表示解析失败
        if (pos + 4 + sz > len) {
            return -1;
        }
        // 将参数的数据添加到结果向量中
        out.push_back(std::string((char *)&data[pos + 4], sz));
        // 更新当前位置，跳过已解析的参数
        pos += 4 + sz;
    }

    // 检查当前位置是否与请求数据的长度相等，如果不相等，则表示有多余的数据，返回-1表示解析失败
    if (pos != len) {
        return -1;  // trailing garbage
    }
    // 返回0表示解析成功
    return 0;
}


enum {
    T_STR = 0,
    T_ZSET = 1,
};

// the structure for the key
struct Entry {
    struct HNode node;
    std::string key;
    std::string val;
    uint32_t type = 0;
    ZSet *zset = NULL;
    // for TTLs
    size_t heap_idx = -1;
};

/**
 * @brief 比较两个 Entry 结构体是否相等
 * 
 * 该函数用于比较两个 Entry 结构体是否相等。它通过比较两个 Entry 结构体中的 key 字段来判断它们是否相等。
 * 
 * @param lhs 指向第一个 Entry 结构体的指针
 * @param rhs 指向第二个 Entry 结构体的指针
 * @return bool 返回 true 表示两个 Entry 结构体相等，返回 false 表示不相等
 */
static bool entry_eq(HNode *lhs, HNode *rhs) {
    // 将 lhs 指针转换为 Entry 结构体指针
    struct Entry *le = container_of(lhs, struct Entry, node);
    // 将 rhs 指针转换为 Entry 结构体指针
    struct Entry *re = container_of(rhs, struct Entry, node);
    // 比较两个 Entry 结构体中的 key 字段是否相等
    return le->key == re->key;
}


enum {
    ERR_UNKNOWN = 1,
    ERR_2BIG = 2,
    ERR_TYPE = 3,
    ERR_ARG = 4,
};

static void out_nil(std::string &out) {
    out.push_back(SER_NIL);
}

static void out_str(std::string &out, const char *s, size_t size) {
    out.push_back(SER_STR);
    uint32_t len = (uint32_t)size;
    out.append((char *)&len, 4);
    out.append(s, len);
}

static void out_str(std::string &out, const std::string &val) {
    return out_str(out, val.data(), val.size());
}

static void out_int(std::string &out, int64_t val) {
    out.push_back(SER_INT);
    out.append((char *)&val, 8);
}

static void out_dbl(std::string &out, double val) {
    // 将类型标识 SER_DBL 添加到字符串中
    out.push_back(SER_DBL);
    // 将双精度浮点数添加到字符串中
    out.append((char *)&val, 8);
}

static void out_err(std::string &out, int32_t code, const std::string &msg) {
    out.push_back(SER_ERR);
    out.append((char *)&code, 4);
    uint32_t len = (uint32_t)msg.size();
    out.append((char *)&len, 4);
    out.append(msg);
}

static void out_arr(std::string &out, uint32_t n) {
    out.push_back(SER_ARR);
    out.append((char *)&n, 4);
}

/**
 * @brief 开始序列化数组
 * 
 * 该函数用于开始序列化一个数组。它会在输出字符串中添加一个数组开始标记，并预留4个字节用于存储数组的长度。
 * 
 * @param out 用于存储序列化数据的字符串
 * @return void* 返回一个指向预留的4个字节的指针，用于在结束序列化数组时填充数组的长度
 */
static void *begin_arr(std::string &out) {
    // 添加数组开始标记
    out.push_back(SER_ARR);
    // 预留4个字节用于存储数组的长度
    out.append("\0\0\0\0", 4);          // filled in end_arr()
    // 返回一个指向预留的4个字节的指针，用于在结束序列化数组时填充数组的长度
    return (void *)(out.size() - 4);    // the `ctx` arg
}


/**
 * @brief 结束序列化数组
 * 
 * 该函数用于结束序列化一个数组。它会在输出字符串中填充数组的长度，并确保数组开始标记正确。
 * 
 * @param out 用于存储序列化数据的字符串
 * @param ctx 指向预留的4个字节的指针，用于填充数组的长度
 * @param n 数组的长度
 */
static void end_arr(std::string &out, void *ctx, uint32_t n) {
    // 获取预留的4个字节的位置
    size_t pos = (size_t)ctx;
    // 确保数组开始标记正确
    assert(out[pos - 1] == SER_ARR);
    // 在预留的4个字节中填充数组的长度
    memcpy(&out[pos], &n, 4);
}

/**
 * @brief 处理GET命令
 * 
 * 该函数用于处理客户端发送的GET命令。它会从数据库中查找指定的键，并返回对应的值。
 * 如果键不存在，则返回空值。如果键的类型不是字符串，则返回错误信息。
 * 
 * @param cmd 包含命令和参数的字符串向量
 * @param out 用于存储返回值的字符串
 */
static void do_get(std::vector<std::string> &cmd, std::string &out) {
    // 创建一个新的Entry对象，并将命令中的键值交换到该对象中
    Entry key;
    key.key.swap(cmd[1]);
    // 计算键的哈希值
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    // 在数据库中查找该键
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    // 如果键不存在，则返回空值
    if (!node) {
        return out_nil(out);
    }

    // 如果键存在，则将HNode指针转换为Entry指针
    Entry *ent = container_of(node, Entry, node);
    // 如果键的类型不是字符串，则返回错误信息
    if (ent->type != T_STR) {
        return out_err(out, ERR_TYPE, "expect string type");
    }
    // 如果键的类型是字符串，则返回该字符串
    return out_str(out, ent->val);
}


/**
 * @brief 处理SET命令
 * 
 * 该函数用于处理客户端发送的SET命令。它会在数据库中查找指定的键，如果键存在，则更新其值；如果键不存在，则创建一个新的键值对。
 * 
 * @param cmd 包含命令和参数的字符串向量
 * @param out 用于存储返回值的字符串
 */
static void do_set(std::vector<std::string> &cmd, std::string &out) {
    // 创建一个新的Entry对象，并将命令中的键值交换到该对象中
    Entry key;
    key.key.swap(cmd[1]);
    // 计算键的哈希值
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    // 在数据库中查找该键
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    // 如果键存在
    if (node) {
        // 将HNode指针转换为Entry指针
        Entry *ent = container_of(node, Entry, node);
        // 如果键的类型不是字符串，则返回错误信息
        if (ent->type != T_STR) {
            return out_err(out, ERR_TYPE, "expect string type");
        }
        // 更新键的值
        ent->val.swap(cmd[2]);
    } 
    // 如果键不存在
    else {
        // 创建一个新的Entry对象
        Entry *ent = new Entry();
        // 将命令中的键值交换到新创建的Entry对象中
        ent->key.swap(key.key);
        // 设置新创建的Entry对象的哈希值
        ent->node.hcode = key.node.hcode;
        // 将命令中的值交换到新创建的Entry对象中
        ent->val.swap(cmd[2]);
        // 将新创建的Entry对象插入到数据库中
        hm_insert(&g_data.db, &ent->node);
    }
    // 返回空值
    return out_nil(out);
}


// set or remove the TTL
/**
 * @brief 设置 Entry 对象的过期时间
 * 
 * 该函数用于设置 Entry 对象的过期时间。如果传入的过期时间为负数，则表示立即过期；如果传入的过期时间为正数，则表示在指定的时间后过期。
 * 
 * @param ent 指向 Entry 对象的指针
 * @param ttl_ms 过期时间，单位为毫秒
 */
static void entry_set_ttl(Entry *ent, int64_t ttl_ms) {
    // 如果过期时间为负数，并且 Entry 对象已经在堆中
    if (ttl_ms < 0 && ent->heap_idx != (size_t)-1) {
        // 通过将其替换为数组中的最后一个元素来从堆中删除一个元素
        size_t pos = ent->heap_idx;
        g_data.heap[pos] = g_data.heap.back();
        g_data.heap.pop_back();
        // 如果位置小于堆的大小，则更新堆
        if (pos < g_data.heap.size()) {
            heap_update(g_data.heap.data(), pos, g_data.heap.size());
        }
        // 将 Entry 对象的堆索引设置为 -1，表示不在堆中
        ent->heap_idx = -1;
    } 
    // 如果过期时间为正数
    else if (ttl_ms >= 0) {
        size_t pos = ent->heap_idx;
        // 如果 Entry 对象不在堆中
        if (pos == (size_t)-1) {
            // 向堆中添加一个新元素
            HeapItem item;
            item.ref = &ent->heap_idx;
            g_data.heap.push_back(item);
            pos = g_data.heap.size() - 1;
        }
        // 设置堆中元素的过期时间
        g_data.heap[pos].val = get_monotonic_usec() + (uint64_t)ttl_ms * 1000;
        // 更新堆
        heap_update(g_data.heap.data(), pos, g_data.heap.size());
    }
}


static bool str2int(const std::string &s, int64_t &out) {
    char *endp = NULL;
    out = strtoll(s.c_str(), &endp, 10);
    return endp == s.c_str() + s.size();
}

/**
 * @brief 处理EXPIRE命令
 * 
 * 该函数用于处理客户端发送的EXPIRE命令。它会在数据库中查找指定的键，并为其设置过期时间。
 * 
 * @param cmd 包含命令和参数的字符串向量
 * @param out 用于存储返回值的字符串
 */
static void do_expire(std::vector<std::string> &cmd, std::string &out) {
    // 定义一个变量，用于存储过期时间（以毫秒为单位）
    int64_t ttl_ms = 0;
    // 将命令中的过期时间转换为整数
    if (!str2int(cmd[2], ttl_ms)) {
        // 如果转换失败，返回错误信息
        return out_err(out, ERR_ARG, "expect int64");
    }

    // 创建一个新的Entry对象，并将命令中的键值交换到该对象中
    Entry key;
    key.key.swap(cmd[1]);
    // 计算键的哈希值
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    // 在数据库中查找该键
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    // 如果键存在
    if (node) {
        // 将HNode指针转换为Entry指针
        Entry *ent = container_of(node, Entry, node);
        // 设置键的过期时间
        entry_set_ttl(ent, ttl_ms);
    }
    // 返回键是否存在的状态
    return out_int(out, node ? 1: 0);
}


/**
 * @brief 处理TTL命令
 * 
 * 该函数用于处理客户端发送的TTL命令。它会在数据库中查找指定的键，并返回该键的剩余过期时间。
 * 如果键不存在，则返回-2。如果键存在但没有设置过期时间，则返回-1。如果键存在且设置了过期时间，则返回剩余过期时间（以毫秒为单位）。
 * 
 * @param cmd 包含命令和参数的字符串向量
 * @param out 用于存储返回值的字符串
 */
static void do_ttl(std::vector<std::string> &cmd, std::string &out) {
    // 创建一个新的Entry对象，并将命令中的键值交换到该对象中
    Entry key;
    key.key.swap(cmd[1]);
    // 计算键的哈希值
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    // 在数据库中查找该键
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    // 如果键不存在，则返回-2
    if (!node) {
        return out_int(out, -2);
    }

    // 如果键存在，则将HNode指针转换为Entry指针
    Entry *ent = container_of(node, Entry, node);
    // 如果键存在但没有设置过期时间，则返回-1
    if (ent->heap_idx == (size_t)-1) {
        return out_int(out, -1);
    }

    // 获取键的过期时间
    uint64_t expire_at = g_data.heap[ent->heap_idx].val;
    // 获取当前时间（以微秒为单位）
    uint64_t now_us = get_monotonic_usec();
    // 计算剩余过期时间（以毫秒为单位）
    return out_int(out, expire_at > now_us ? (expire_at - now_us) / 1000 : 0);
}


// deallocate the key immediately
static void entry_destroy(Entry *ent) {
    switch (ent->type) {
    case T_ZSET:
        zset_dispose(ent->zset);
        delete ent->zset;
        break;
    }
    delete ent;
}

/**
 * 异步销毁 Entry 对象
 * @param arg 指向要销毁的 Entry 对象的指针
 */
static void entry_del_async(void *arg) {
    // 将参数转换为 Entry 类型的指针
    Entry *ent = (Entry *)arg;
    // 销毁 Entry 对象
    entry_destroy(ent);
}


// dispose the entry after it got detached from the key space
/**
 * 销毁 Entry 对象
 * @param ent 指向要销毁的 Entry 对象的指针
 */
static void entry_del(Entry *ent) {
    // 设置 Entry 对象的过期时间为 -1，表示立即过期
    entry_set_ttl(ent, -1);

    // 定义一个常量，表示大型容器的大小阈值
    const size_t k_large_container_size = 10000;
    // 标记是否为大型容器
    bool too_big = false;
    // 根据 Entry 对象的类型进行不同的处理
    switch (ent->type) {
    // 如果是 T_ZSET 类型（有序集合）
    case T_ZSET:
        // 判断有序集合的哈希表大小是否超过阈值
        too_big = hm_size(&ent->zset->hmap) > k_large_container_size;
        break;
    }

    // 如果是大型容器
    if (too_big) {
        // 将销毁任务加入线程池队列，异步执行
        thread_pool_queue(&g_data.tp, &entry_del_async, ent);
    } 
    // 如果不是大型容器
    else {
        // 直接销毁 Entry 对象
        entry_destroy(ent);
    }
}


/**
 * @brief 处理DEL命令
 * 
 * 该函数用于处理客户端发送的DEL命令。它会在数据库中查找指定的键，并将其删除。
 * 
 * @param cmd 包含命令和参数的字符串向量
 * @param out 用于存储返回值的字符串
 */
static void do_del(std::vector<std::string> &cmd, std::string &out) {
    // 创建一个新的Entry对象，并将命令中的键值交换到该对象中
    Entry key;
    key.key.swap(cmd[1]);
    // 计算键的哈希值
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    // 在数据库中查找并删除该键
    HNode *node = hm_pop(&g_data.db, &key.node, &entry_eq);
    // 如果键存在
    if (node) {
        // 将HNode指针转换为Entry指针，并调用entry_del函数删除该Entry对象
        entry_del(container_of(node, Entry, node));
    }
    // 返回键是否存在的状态
    return out_int(out, node ? 1: 0);
}


/**
 * @brief 扫描哈希表中的所有节点，并对每个节点执行指定的回调函数
 * 
 * 该函数用于扫描哈希表中的所有节点，并对每个节点执行指定的回调函数。回调函数会在每个节点上被调用，并且可以通过参数传递额外的信息。
 * 
 * @param tab 指向要扫描的哈希表的指针
 * @param f 指向回调函数的指针，该函数会在每个节点上被调用
 * @param arg 传递给回调函数的额外参数
 */
static void h_scan(HTab *tab, void (*f)(HNode *, void *), void *arg) {
    // 如果哈希表的大小为0，则直接返回
    if (tab->size == 0) {
        return;
    }
    // 遍历哈希表的每个桶
    for (size_t i = 0; i < tab->mask + 1; ++i) {
        // 获取当前桶的第一个节点
        HNode *node = tab->tab[i];
        // 遍历当前桶的所有节点
        while (node) {
            // 对当前节点执行回调函数
            f(node, arg);
            // 获取下一个节点
            node = node->next;
        }
    }
}


/**
 * @brief 扫描哈希表中的节点并将其键添加到输出字符串中
 * 
 * 该函数用于扫描哈希表中的每个节点，并将节点对应的键添加到输出字符串中。
 * 
 * @param node 指向当前扫描的哈希节点的指针
 * @param arg 指向输出字符串的指针，该字符串用于存储扫描到的键
 */
static void cb_scan(HNode *node, void *arg) {
    // 将参数转换为指向字符串的指针
    std::string &out = *(std::string *)arg;
    // 将当前节点的键添加到输出字符串中
    out_str(out, container_of(node, Entry, node)->key);
}


/**
 * @brief 处理KEYS命令
 * 
 * 该函数用于处理客户端发送的KEYS命令。它会扫描数据库中的所有键，并将它们添加到输出字符串中。
 * 
 * @param cmd 包含命令和参数的字符串向量
 * @param out 用于存储返回值的字符串
 */
static void do_keys(std::vector<std::string> &cmd, std::string &out) {
    // 忽略命令参数，因为KEYS命令不需要参数
    (void)cmd;
    // 开始序列化一个数组，数组的长度为数据库中键的数量
    out_arr(out, (uint32_t)hm_size(&g_data.db));
    // 扫描第一个哈希表，并将扫描到的键添加到输出字符串中
    h_scan(&g_data.db.ht1, &cb_scan, &out);
    // 扫描第二个哈希表，并将扫描到的键添加到输出字符串中
    h_scan(&g_data.db.ht2, &cb_scan, &out);
}


/**
 * @brief 将字符串转换为双精度浮点数
 * 
 * 该函数用于将字符串转换为双精度浮点数。它会调用标准库函数strtod进行转换，并检查转换是否成功。
 * 
 * @param s 要转换的字符串
 * @param out 用于存储转换结果的变量
 * @return 如果转换成功，则返回true；否则返回false
 */
static bool str2dbl(const std::string &s, double &out) {
    // 定义一个指针，用于存储转换结束的位置
    char *endp = NULL;
    // 调用strtod函数进行转换，并将转换结束的位置存储在endp中
    out = strtod(s.c_str(), &endp);
    // 检查转换是否成功
    return endp == s.c_str() + s.size() && !isnan(out);
}


// zadd zset score name
/**
 * @brief 处理ZADD命令
 * 
 * 该函数用于处理客户端发送的ZADD命令。它会在数据库中查找指定的有序集合（ZSET），如果不存在则创建一个新的ZSET，然后将指定的成员和分数添加到ZSET中。
 * 
 * @param cmd 包含命令和参数的字符串向量
 * @param out 用于存储返回值的字符串
 */
static void do_zadd(std::vector<std::string> &cmd, std::string &out) {
    // 定义一个变量，用于存储分数
    double score = 0;
    // 将命令中的分数转换为双精度浮点数
    if (!str2dbl(cmd[2], score)) {
        // 如果转换失败，返回错误信息
        return out_err(out, ERR_ARG, "expect fp number");
    }

    // 查找或创建ZSET
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    HNode *hnode = hm_lookup(&g_data.db, &key.node, &entry_eq);

    Entry *ent = NULL;
    // 如果ZSET不存在，则创建一个新的ZSET
    if (!hnode) {
        ent = new Entry();
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        ent->type = T_ZSET;
        ent->zset = new ZSet();
        hm_insert(&g_data.db, &ent->node);
    } 
    // 如果ZSET存在
    else {
        // 将HNode指针转换为Entry指针
        ent = container_of(hnode, Entry, node);
        // 如果ZSET的类型不是T_ZSET，则返回错误信息
        if (ent->type != T_ZSET) {
            return out_err(out, ERR_TYPE, "expect zset");
        }
    }

    // 添加或更新元组
    const std::string &name = cmd[3];
    // 将成员和分数添加到ZSET中
    bool added = zset_add(ent->zset, name.data(), name.size(), score);
    // 返回是否成功添加成员
    return out_int(out, (int64_t)added);
}


/**
 * @brief 期望并验证给定键对应的条目是一个有序集合（ZSET）
 * 
 * 该函数用于检查给定的键是否存在于数据库中，并且对应的条目类型是否为有序集合（ZSET）。
 * 如果键不存在或条目类型不是ZSET，则返回错误信息。
 * 
 * @param out 用于存储返回值的字符串
 * @param s 要查找的键
 * @param ent 指向Entry指针的指针，用于存储找到的条目
 * @return 如果键存在且条目类型为ZSET，则返回true；否则返回false
 */
static bool expect_zset(std::string &out, std::string &s, Entry **ent) {
    // 创建一个新的Entry对象，并将键值交换到该对象中
    Entry key;
    key.key.swap(s);
    // 计算键的哈希值
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    // 在数据库中查找该键
    HNode *hnode = hm_lookup(&g_data.db, &key.node, &entry_eq);
    // 如果键不存在
    if (!hnode) {
        // 返回空值
        out_nil(out);
        return false;
    }

    // 将HNode指针转换为Entry指针
    *ent = container_of(hnode, Entry, node);
    // 如果条目类型不是ZSET
    if ((*ent)->type != T_ZSET) {
        // 返回错误信息
        out_err(out, ERR_TYPE, "expect zset");
        return false;
    }
    // 如果键存在且条目类型为ZSET，则返回true
    return true;
}


// zrem zset name
/**
 * @brief 处理ZREM命令
 * 
 * 该函数用于处理客户端发送的ZREM命令。它会在数据库中查找指定的有序集合（ZSET），并从中删除指定的成员。
 * 
 * @param cmd 包含命令和参数的字符串向量
 * @param out 用于存储返回值的字符串
 */
static void do_zrem(std::vector<std::string> &cmd, std::string &out) {
    // 查找指定的有序集合（ZSET）
    Entry *ent = NULL;
    if (!expect_zset(out, cmd[1], &ent)) {
        // 如果找不到指定的ZSET，则返回错误信息
        return;
    }

    // 获取要删除的成员名称
    const std::string &name = cmd[2];
    // 从ZSET中删除指定的成员
    ZNode *znode = zset_pop(ent->zset, name.data(), name.size());
    if (znode) {
        // 如果成功删除成员，则释放内存
        znode_del(znode);
    }
    // 返回是否成功删除成员
    return out_int(out, znode ? 1 : 0);
}


// zscore zset name
/**
 * @brief 处理ZSCORE命令
 * 
 * 该函数用于处理客户端发送的ZSCORE命令。它会在数据库中查找指定的有序集合（ZSET），并返回指定成员的分数。
 * 
 * @param cmd 包含命令和参数的字符串向量
 * @param out 用于存储返回值的字符串
 */
static void do_zscore(std::vector<std::string> &cmd, std::string &out) {
    // 查找指定的有序集合（ZSET）
    Entry *ent = NULL;
    if (!expect_zset(out, cmd[1], &ent)) {
        // 如果找不到指定的ZSET，则返回错误信息
        return;
    }

    // 获取要查找的成员名称
    const std::string &name = cmd[2];
    // 在ZSET中查找指定的成员
    ZNode *znode = zset_lookup(ent->zset, name.data(), name.size());
    // 如果找到成员，则返回其分数；否则返回空值
    return znode ? out_dbl(out, znode->score) : out_nil(out);
}


// zquery zset score name offset limit
/**
 * @brief 处理ZQUERY命令
 * 
 * 该函数用于处理客户端发送的ZQUERY命令。它会在数据库中查找指定的有序集合（ZSET），并根据给定的分数、成员名称、偏移量和限制返回匹配的成员及其分数。
 * 
 * @param cmd 包含命令和参数的字符串向量
 * @param out 用于存储返回值的字符串
 */
static void do_zquery(std::vector<std::string> &cmd, std::string &out) {
    // 解析命令参数
    double score = 0;
    // 将命令中的分数转换为双精度浮点数
    if (!str2dbl(cmd[2], score)) {
        // 如果转换失败，返回错误信息
        return out_err(out, ERR_ARG, "expect fp number");
    }
    // 获取要查找的成员名称
    const std::string &name = cmd[3];
    int64_t offset = 0;
    int64_t limit = 0;
    // 将命令中的偏移量转换为整数
    if (!str2int(cmd[4], offset)) {
        // 如果转换失败，返回错误信息
        return out_err(out, ERR_ARG, "expect int");
    }
    // 将命令中的限制转换为整数
    if (!str2int(cmd[5], limit)) {
        // 如果转换失败，返回错误信息
        return out_err(out, ERR_ARG, "expect int");
    }

    // 获取指定的有序集合（ZSET）
    Entry *ent = NULL;
    if (!expect_zset(out, cmd[1], &ent)) {
        // 如果找不到指定的ZSET，则返回空数组
        if (out[0] == SER_NIL) {
            out.clear();
            out_arr(out, 0);
        }
        return;
    }

    // 在ZSET中查找匹配的成员
    if (limit <= 0) {
        // 如果限制小于等于0，则返回空数组
        return out_arr(out, 0);
    }
    // 在ZSET中查找指定分数和成员名称的节点
    ZNode *znode = zset_query(ent->zset, score, name.data(), name.size());
    // 根据偏移量调整节点指针
    znode = znode_offset(znode, offset);

    // 输出匹配的成员及其分数
    void *arr = begin_arr(out);
    uint32_t n = 0;
    // 遍历匹配的节点，并将其名称和分数添加到输出字符串中
    while (znode && (int64_t)n < limit) {
        out_str(out, znode->name, znode->len);
        out_dbl(out, znode->score);
        // 移动到下一个节点
        znode = znode_offset(znode, +1);
        n += 2;
    }
    // 结束数组输出
    end_arr(out, arr, n);
}


static bool cmd_is(const std::string &word, const char *cmd) {
    return 0 == strcasecmp(word.c_str(), cmd);
}

static void do_request(std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() == 1 && cmd_is(cmd[0], "keys")) {
        do_keys(cmd, out);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "get")) {
        do_get(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "set")) {
        do_set(cmd, out);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "del")) {
        do_del(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "pexpire")) {
        do_expire(cmd, out);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "pttl")) {
        do_ttl(cmd, out);
    } else if (cmd.size() == 4 && cmd_is(cmd[0], "zadd")) {
        do_zadd(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "zrem")) {
        do_zrem(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "zscore")) {
        do_zscore(cmd, out);
    } else if (cmd.size() == 6 && cmd_is(cmd[0], "zquery")) {
        do_zquery(cmd, out);
    } else {
        // cmd is not recognized
        out_err(out, ERR_UNKNOWN, "Unknown cmd");
    }
}

/**
 * @brief 尝试从缓冲区解析一个请求
 * 
 * 该函数用于尝试从缓冲区解析一个请求。如果缓冲区中的数据不足以解析一个完整的请求，
 * 则返回false，等待下一次迭代。如果解析成功，则生成响应并将其打包到缓冲区中，然后
 * 从缓冲区中移除已处理的请求。最后，改变连接的状态为STATE_RES，并调用state_res函数
 * 尝试将响应发送到客户端。如果请求被完全处理，则返回true，否则返回false。
 * 
 * @param conn 指向连接对象的指针
 * @return 如果请求被完全处理，则返回true；否则返回false
 */
static bool try_one_request(Conn *conn) {
    // try to parse a request from the buffer
    if (conn->rbuf_size < 4) {
        // not enough data in the buffer. Will retry in the next iteration
        return false;
    }
    uint32_t len = 0;
    memcpy(&len, &conn->rbuf[0], 4);
    if (len > k_max_msg) {
        msg("too long");
        conn->state = STATE_END;
        return false;
    }
    if (4 + len > conn->rbuf_size) {
        // not enough data in the buffer. Will retry in the next iteration
        return false;
    }

    // parse the request
    std::vector<std::string> cmd;
    if (0 != parse_req(&conn->rbuf[4], len, cmd)) {
        msg("bad req");
        conn->state = STATE_END;
        return false;
    }

    // got one request, generate the response.
    std::string out;
    do_request(cmd, out);

    // pack the response into the buffer
    if (4 + out.size() > k_max_msg) {
        out.clear();
        out_err(out, ERR_2BIG, "response is too big");
    }
    uint32_t wlen = (uint32_t)out.size();
    memcpy(&conn->wbuf[0], &wlen, 4);
    memcpy(&conn->wbuf[4], out.data(), out.size());
    conn->wbuf_size = 4 + wlen;

    // remove the request from the buffer.
    // note: frequent memmove is inefficient.
    // note: need better handling for production code.
    size_t remain = conn->rbuf_size - 4 - len;
    if (remain) {
        memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
    }
    conn->rbuf_size = remain;

    // change state
    conn->state = STATE_RES;
    state_res(conn);

    // continue the outer loop if the request was fully processed
    return (conn->state == STATE_REQ);
}


/**
 * @brief 尝试填充缓冲区
 * 
 * 该函数用于尝试从连接的文件描述符中读取数据并填充到缓冲区中。如果读取成功，
 * 则继续尝试解析并处理请求，直到所有请求都被处理完毕或者缓冲区中没有足够的数据。
 * 
 * @param conn 指向连接对象的指针
 * @return 如果连接的状态为STATE_REQ，则返回true；否则返回false
 */
static bool try_fill_buffer(Conn *conn) {
    // try to fill the buffer
    assert(conn->rbuf_size < sizeof(conn->rbuf));
    ssize_t rv = 0;
    do {
        size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
        rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
    } while (rv < 0 && errno == EINTR);
    if (rv < 0 && errno == EAGAIN) {
        // got EAGAIN, stop.
        return false;
    }
    if (rv < 0) {
        msg("read() error");
        conn->state = STATE_END;
        return false;
    }
    if (rv == 0) {
        if (conn->rbuf_size > 0) {
            msg("unexpected EOF");
        } else {
            msg("EOF");
        }
        conn->state = STATE_END;
        return false;
    }

    conn->rbuf_size += (size_t)rv;
    assert(conn->rbuf_size <= sizeof(conn->rbuf));

    // Try to process requests one by one.
    // Why is there a loop? Please read the explanation of "pipelining".
    while (try_one_request(conn)) {}
    return (conn->state == STATE_REQ);
}


static void state_req(Conn *conn) {
    while (try_fill_buffer(conn)) {}
}

/**
 * @brief 尝试刷新缓冲区
 * 
 * 该函数用于尝试将缓冲区中的数据发送到连接的文件描述符中。如果发送成功，
 * 则继续尝试发送，直到所有数据都被发送完毕或者缓冲区中没有更多的数据。
 * 
 * @param conn 指向连接对象的指针
 * @return 如果缓冲区中还有数据需要发送，则返回true；否则返回false
 */
static bool try_flush_buffer(Conn *conn) {
    ssize_t rv = 0;
    do {
        // 计算缓冲区中剩余需要发送的数据量
        size_t remain = conn->wbuf_size - conn->wbuf_sent;
        // 尝试将剩余数据发送到连接的文件描述符中
        rv = write(conn->fd, &conn->wbuf[conn->wbuf_sent], remain);
    } while (rv < 0 && errno == EINTR);
    if (rv < 0 && errno == EAGAIN) {
        // 如果发送被EAGAIN信号中断，停止发送
        return false;
    }
    if (rv < 0) {
        // 如果发送过程中发生错误，记录错误信息并结束连接
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }
    // 更新已发送数据的大小
    conn->wbuf_sent += (size_t)rv;
    assert(conn->wbuf_sent <= conn->wbuf_size);
    if (conn->wbuf_sent == conn->wbuf_size) {
        // 如果所有数据都已发送完毕，改变连接的状态并重置缓冲区
        conn->state = STATE_REQ;
        conn->wbuf_sent = 0;
        conn->wbuf_size = 0;
        return false;
    }
    // 如果缓冲区中还有数据需要发送，返回true
    return true;
}


static void state_res(Conn *conn) {
    while (try_flush_buffer(conn)) {}
}

/**
 * @brief 处理连接的I/O操作
 * 
 * 该函数用于处理连接的I/O操作。它会在连接被poll唤醒时更新空闲定时器，
 * 并根据连接的状态调用相应的处理函数。
 * 
 * @param conn 指向连接对象的指针
 */
static void connection_io(Conn *conn) {
    // waked up by poll, update the idle timer
    // by moving conn to the end of the list.
    conn->idle_start = get_monotonic_usec();
    dlist_detach(&conn->idle_list);
    dlist_insert_before(&g_data.idle_list, &conn->idle_list);

    // do the work
    if (conn->state == STATE_REQ) {
        state_req(conn);
    } else if (conn->state == STATE_RES) {
        state_res(conn);
    } else {
        assert(0);  // not expected
    }
}


const uint64_t k_idle_timeout_ms = 5 * 1000;

/**
 * @brief 计算下一个定时器的超时时间（以毫秒为单位）
 * 
 * 该函数用于计算下一个定时器的超时时间。它会检查空闲连接列表和堆中的定时器，
 * 并返回最早到期的定时器的超时时间。如果没有定时器，则返回10000毫秒。
 * 
 * @return 下一个定时器的超时时间（以毫秒为单位）
 */
static uint32_t next_timer_ms() {
    // 获取当前时间（以微秒为单位）
    uint64_t now_us = get_monotonic_usec();
    // 初始化下一个定时器的超时时间为最大可能值
    uint64_t next_us = (uint64_t)-1;

    // 检查空闲连接列表中的定时器
    if (!dlist_empty(&g_data.idle_list)) {
        // 获取下一个空闲连接
        Conn *next = container_of(g_data.idle_list.next, Conn, idle_list);
        // 计算下一个空闲连接的超时时间
        next_us = next->idle_start + k_idle_timeout_ms * 1000;
    }

    // 检查堆中的定时器
    if (!g_data.heap.empty() && g_data.heap[0].val < next_us) {
        // 更新下一个定时器的超时时间为堆中最早到期的定时器的超时时间
        next_us = g_data.heap[0].val;
    }

    // 如果没有定时器，则返回10000毫秒
    if (next_us == (uint64_t)-1) {
        return 10000;   // no timer, the value doesn't matter
    }

    // 如果下一个定时器已经过期，则返回0毫秒
    if (next_us <= now_us) {
        // missed?
        return 0;
    }
    // 返回下一个定时器的超时时间（以毫秒为单位）
    return (uint32_t)((next_us - now_us) / 1000);
}


/**
 * @brief 关闭并释放连接
 * 
 * 该函数用于关闭并释放一个连接对象。它会从全局的文件描述符到连接的映射中移除该连接，
 * 关闭连接的文件描述符，从空闲连接列表中移除该连接，并释放连接对象的内存。
 * 
 * @param conn 指向连接对象的指针
 */
static void conn_done(Conn *conn) {
    // 从全局的文件描述符到连接的映射中移除该连接
    g_data.fd2conn[conn->fd] = NULL;
    // 关闭连接的文件描述符
    (void)close(conn->fd);
    // 从空闲连接列表中移除该连接
    dlist_detach(&conn->idle_list);
    // 释放连接对象的内存
    free(conn);
}


static bool hnode_same(HNode *lhs, HNode *rhs) {
    return lhs == rhs;
}

/**
 * @brief 处理定时器事件
 * 
 * 该函数用于处理定时器事件。它会检查空闲连接列表和堆中的定时器，
 * 并关闭超时的空闲连接或删除过期的键值对。
 */
static void process_timers() {
    // 获取当前时间（微秒），并额外增加 1000 微秒以适应 poll() 的毫秒分辨率
    uint64_t now_us = get_monotonic_usec() + 1000;

    // 处理空闲定时器
    while (!dlist_empty(&g_data.idle_list)) {
        // 获取空闲定时器列表中的下一个连接
        Conn *next = container_of(g_data.idle_list.next, Conn, idle_list);
        // 计算下一个连接的空闲超时时间（微秒）
        uint64_t next_us = next->idle_start + k_idle_timeout_ms * 1000;
        // 如果下一个连接的空闲超时时间未到，停止处理空闲定时器
        if (next_us >= now_us) {
            break;
        }

        // 打印日志，表示正在移除空闲连接
        printf("removing idle connection: %d\n", next->fd);
        // 关闭并释放空闲连接
        conn_done(next);
    }

    // 处理 TTL 定时器
    const size_t k_max_works = 2000;
    size_t nworks = 0;
    // 当 TTL 定时器堆不为空且堆顶元素的超时时间小于当前时间时，循环处理
    while (!g_data.heap.empty() && g_data.heap[0].val < now_us) {
        // 获取堆顶元素对应的 Entry 对象
        Entry *ent = container_of(g_data.heap[0].ref, Entry, heap_idx);
        // 从哈希表中移除该 Entry 对象对应的 HNode
        HNode *node = hm_pop(&g_data.db, &ent->node, &hnode_same);
        // 断言移除的 HNode 就是当前 Entry 对象对应的 HNode
        assert(node == &ent->node);
        // 删除 Entry 对象并释放其内存
        entry_del(ent);
        // 限制每次处理的最大工作量，避免服务器因大量键同时过期而阻塞
        if (nworks++ >= k_max_works) {
            break;
        }
    }
}


/**
 * @brief 程序的主函数
 * 
 * 该函数用于启动Redis服务器。它会创建一个监听套接字，绑定到指定的端口，
 * 并开始监听连接请求。然后，它会进入一个事件循环，不断地处理连接请求、
 * 读写数据、处理定时器事件等。
 * 
 * @return 程序的返回值，通常为0表示正常退出
 */
int main() {
    // prepare the listening socket
    // 创建一个监听套接字
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        // 如果创建失败，输出错误信息并退出程序
        die("socket()");
    }

    // 设置套接字选项，允许重用地址
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // bind
    // 绑定套接字到指定的地址和端口
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(0);    // wildcard address 0.0.0.0
    int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));
    if (rv) {
        // 如果绑定失败，输出错误信息并退出程序
        die("bind()");
    }

    // listen
    // 开始监听连接请求
    rv = listen(fd, SOMAXCONN);
    if (rv) {
        // 如果监听失败，输出错误信息并退出程序
        die("listen()");
    }

    // set the listen fd to nonblocking mode
    // 设置监听套接字为非阻塞模式
    fd_set_nb(fd);

    // some initializations
    // 初始化空闲连接列表和线程池
    dlist_init(&g_data.idle_list);
    thread_pool_init(&g_data.tp, 4);

    // the event loop
    // 事件循环
    std::vector<struct pollfd> poll_args;
    while (true) {
        // prepare the arguments of the poll()
        // 准备poll函数的参数
        poll_args.clear();
        // for convenience, the listening fd is put in the first position
        // 为了方便，将监听套接字放在第一个位置
        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);
        // connection fds
        // 遍历所有连接，将其文件描述符和事件添加到poll参数中
        for (Conn *conn : g_data.fd2conn) {
            if (!conn) {
                continue;
            }
            struct pollfd pfd = {};
            pfd.fd = conn->fd;
            pfd.events = (conn->state == STATE_REQ) ? POLLIN : POLLOUT;
            pfd.events = pfd.events | POLLERR;
            poll_args.push_back(pfd);
        }

        // poll for active fds
        // 调用poll函数，等待事件发生
        int timeout_ms = (int)next_timer_ms();
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), timeout_ms);
        if (rv < 0) {
            // 如果poll调用失败，输出错误信息并退出程序
            die("poll");
        }

        // process active connections
        // 处理活跃的连接
        for (size_t i = 1; i < poll_args.size(); ++i) {
            if (poll_args[i].revents) {
                Conn *conn = g_data.fd2conn[poll_args[i].fd];
                connection_io(conn);
                if (conn->state == STATE_END) {
                    // client closed normally, or something bad happened.
                    // destroy this connection
                    // 如果连接状态为STATE_END，表示客户端正常关闭或发生错误，销毁连接
                    conn_done(conn);
                }
            }
        }

        // handle timers
        // 处理定时器事件
        process_timers();

        // try to accept a new connection if the listening fd is active
        // 如果监听套接字有事件发生，尝试接受新的连接
        if (poll_args[0].revents) {
            (void)accept_new_conn(fd);
        }
    }

    return 0;
}

