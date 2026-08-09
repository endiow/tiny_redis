// 引入必要的头文件
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <string>
#include <vector>
#include <iostream>
// 引入项目的公共头文件
#include "common.h"

/**
 * @brief 打印错误信息
 * 
 * @param msg 错误信息字符串
 */
static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

/**
 * @brief 打印错误信息并终止程序
 * 
 * @param msg 错误信息字符串
 */
static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

/**
 * @brief 从文件描述符中读取指定长度的数据
 * 
 * @param fd 文件描述符
 * @param buf 存储读取数据的缓冲区
 * @param n 要读取的字节数
 * @return int32_t 读取成功返回0，否则返回-1
 */
static int32_t read_full(int fd, char *buf, size_t n) {
    while (n > 0) {
        // 读取数据
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) {
            return -1;  // error, or unexpected EOF
        }
        // 确保读取的字节数不超过请求的字节数
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}


/**
 * @brief 将指定长度的数据写入文件描述符
 * 
 * 该函数尝试将指定长度的数据从缓冲区写入文件描述符。如果写入过程中发生错误，或者写入的字节数小于请求的字节数，函数将返回-1。
 * 
 * @param fd 文件描述符
 * @param buf 存储要写入数据的缓冲区
 * @param n 要写入的字节数
 * @return int32_t 写入成功返回0，否则返回-1
 */
static int32_t write_all(int fd, const char *buf, size_t n) {
    // 循环直到所有数据都被写入
    while (n > 0) {
        // 尝试写入数据
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) {
            return -1;  // error
        }
        // 确保写入的字节数不超过请求的字节数
        assert((size_t)rv <= n);
        // 更新剩余要写入的字节数
        n -= (size_t)rv;
        // 更新缓冲区指针
        buf += rv;
    }
    return 0;
}


const size_t k_max_msg = 4096;

/**
 * @brief 将命令发送到指定的文件描述符
 * 
 * 该函数将命令及其参数序列化为一个缓冲区，并将其发送到指定的文件描述符。如果命令的总长度超过了最大消息长度，函数将返回-1。
 * 
 * @param fd 文件描述符
 * @param cmd 包含命令及其参数的字符串向量
 * @return int32_t 发送成功返回0，否则返回-1
 */
static int32_t send_req(int fd, const std::vector<std::string> &cmd) {
    // 计算消息总长度
    uint32_t len = 4;
    for (const std::string &s : cmd) {
        len += 4 + s.size();
    }
    // 检查消息长度是否超过最大限制
    if (len > k_max_msg) {
        return -1;
    }

    // 分配缓冲区
    char wbuf[4 + k_max_msg];
    // 将消息长度写入缓冲区
    memcpy(&wbuf[0], &len, 4);  // assume little endian
    // 将命令数量写入缓冲区
    uint32_t n = cmd.size();
    memcpy(&wbuf[4], &n, 4);
    // 当前写入位置
    size_t cur = 8;
    // 将每个命令及其参数写入缓冲区
    for (const std::string &s : cmd) {
        uint32_t p = (uint32_t)s.size();
        memcpy(&wbuf[cur], &p, 4);
        memcpy(&wbuf[cur + 4], s.data(), s.size());
        cur += 4 + s.size();
    }
    // 将缓冲区内容发送到文件描述符
    return write_all(fd, wbuf, 4 + len);
}


/**
 * @brief 处理接收到的响应数据
 * 
 * 该函数根据响应数据的第一个字节来确定响应的类型，并根据类型进行相应的处理。如果响应数据格式不正确，函数将返回-1。
 * 
 * @param data 指向响应数据的指针
 * @param size 响应数据的大小
 * @return int32_t 处理成功返回已处理的字节数，否则返回-1
 */
static int32_t on_response(const uint8_t *data, size_t size) {
    // 检查响应数据大小是否至少为1字节
    if (size < 1) {
        msg("bad response");
        return -1;
    }
    // 根据响应数据的第一个字节进行类型判断
    switch (data[0]) {
    case SER_NIL:
        // 处理空值响应
        printf("(nil)\n");
        return 1;
    case SER_ERR:
        // 处理错误响应
        if (size < 1 + 8) {
            msg("bad response");
            return -1;
        }
        {
            int32_t code = 0;
            uint32_t len = 0;
            // 解析错误代码和错误消息长度
            memcpy(&code, &data[1], 4);
            memcpy(&len, &data[1 + 4], 4);
            // 检查响应数据大小是否足够包含错误消息
            if (size < 1 + 8 + len) {
                msg("bad response");
                return -1;
            }
            // 打印错误信息
            printf("(err) %d %.*s\n", code, len, &data[1 + 8]);
            return 1 + 8 + len;
        }
    case SER_STR:
        // 处理字符串响应
        if (size < 1 + 4) {
            msg("bad response");
            return -1;
        }
        {
            uint32_t len = 0;
            // 解析字符串长度
            memcpy(&len, &data[1], 4);
            // 检查响应数据大小是否足够包含字符串
            if (size < 1 + 4 + len) {
                msg("bad response");
                return -1;
            }
            // 打印字符串
            printf("(str) %.*s\n", len, &data[1 + 4]);
            return 1 + 4 + len;
        }
    case SER_INT:
        // 处理整数响应
        if (size < 1 + 8) {
            msg("bad response");
            return -1;
        }
        {
            int64_t val = 0;
            // 解析整数值
            memcpy(&val, &data[1], 8);
            // 打印整数值
            printf("(int) %ld\n", val);
            return 1 + 8;
        }
    case SER_DBL:
        // 处理浮点数响应
        if (size < 1 + 8) {
            msg("bad response");
            return -1;
        }
        {
            double val = 0;
            // 解析浮点数值
            memcpy(&val, &data[1], 8);
            // 打印浮点数值
            printf("(dbl) %g\n", val);
            return 1 + 8;
        }
    case SER_ARR:
        // 处理数组响应
        if (size < 1 + 4) {
            msg("bad response");
            return -1;
        }
        {
            uint32_t len = 0;
            // 解析数组长度
            memcpy(&len, &data[1], 4);
            // 打印数组长度
            printf("(arr) len=%u\n", len);
            size_t arr_bytes = 1 + 4;
            for (uint32_t i = 0; i < len; ++i) {
                int32_t rv = on_response(&data[arr_bytes], size - arr_bytes);
                if (rv < 0) {
                    return rv;
                }
                arr_bytes += (size_t)rv;
            }
            printf("(arr) end\n");
            return (int32_t)arr_bytes;
        }
    default:
        // 处理未知类型的响应
        msg("bad response");
        return -1;
    }
}


/**
 * @brief 从文件描述符中读取并处理响应数据
 * 
 * 该函数首先读取响应头（4字节），解析出响应数据的长度。然后读取相应长度的响应数据，并调用 `on_response` 函数进行处理。
 * 
 * @param fd 文件描述符
 * @return int32_t 处理成功返回已处理的字节数，否则返回-1
 */
static int32_t read_res(int fd) {
    // 4 bytes header
    // 定义一个缓冲区，用于存储响应头和响应体
    char rbuf[4 + k_max_msg + 1];
    // 重置 errno 以检查错误
    errno = 0;
    // 读取响应头（4字节）
    int32_t err = read_full(fd, rbuf, 4);
    if (err) {
        // 检查是否是 EOF 或其他错误
        if (errno == 0) {
            msg("EOF");
        } else {
            msg("read() error");
        }
        return err;
    }

    // 解析响应头，获取响应体的长度
    uint32_t len = 0;
    memcpy(&len, rbuf, 4);  // assume little endian
    if (len > k_max_msg) {
        msg("too long");
        return -1;
    }

    // reply body
    // 读取响应体
    err = read_full(fd, &rbuf[4], len);
    if (err) {
        msg("read() error");
        return err;
    }

    // print the result
    // 处理响应数据
    int32_t rv = on_response((uint8_t *)&rbuf[4], len);
    if (rv > 0 && (uint32_t)rv != len) {
        msg("bad response");
        rv = -1;
    }
    return rv;
}


/**
 * @brief 程序的主函数，用于连接到服务器并发送命令
 * 
 * @param argc 命令行参数的数量
 * @param argv 命令行参数的数组
 * @return int 程序的返回值
 */
int main(int argc, char **argv) {
    // 创建一个套接字
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        // 如果创建套接字失败，打印错误信息并终止程序
        die("socket()");
    }

    // 配置服务器地址
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(INADDR_LOOPBACK);  // 127.0.0.1

    // 连接到服务器
    int rv = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) {
        // 如果连接失败，打印错误信息并终止程序
        die("connect");
    }

    // 从命令行参数中提取命令
    std::vector<std::string> cmd;
    for (int i = 1; i < argc; ++i) {
        cmd.push_back(argv[i]);
    }

    // 发送命令到服务器
    int32_t err = send_req(fd, cmd);
    if (err) {
        // 如果发送命令失败，跳转到清理代码
        goto L_DONE;
    }

    // 从服务器读取响应
    err = read_res(fd);
    if (err) {
        // 如果读取响应失败，跳转到清理代码
        goto L_DONE;
    }

L_DONE:
    // 关闭套接字
    close(fd);
    return 0;
}

