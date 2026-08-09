#include <assert.h>
#include "thread_pool.h"


/**
 * 工作线程函数，负责从任务队列中获取任务并执行
 * @param arg 线程池对象的指针
 * @return NULL
 */
static void *worker(void *arg) {
    // 将传入的参数转换为线程池对象指针
    TheadPool *tp = (TheadPool *)arg;
    // 无限循环，持续从任务队列中获取任务并执行
    while (true) {
        // 加锁，保护任务队列的访问
        pthread_mutex_lock(&tp->mu);
        // 等待条件变量：任务队列不为空
        while (tp->queue.empty()) {
            // 等待条件变量，释放互斥锁，等待条件满足后重新加锁
            pthread_cond_wait(&tp->not_empty, &tp->mu);
        }
        // 获取任务队列中的第一个任务
        Work w = tp->queue.front();
        // 从任务队列中移除第一个任务
        tp->queue.pop_front();
        // 解锁，允许其他线程访问任务队列
        pthread_mutex_unlock(&tp->mu);
        // 执行任务
        w.f(w.arg);
    }
    // 线程函数返回 NULL
    return NULL;
}


/**
 * 初始化线程池
 * @param tp 线程池对象指针
 * @param num_threads 线程池中线程的数量
 */
void thread_pool_init(TheadPool *tp, size_t num_threads) {
    // 断言线程数量大于 0
    assert(num_threads > 0);

    // 初始化互斥锁
    int rv = pthread_mutex_init(&tp->mu, NULL);
    // 断言互斥锁初始化成功
    assert(rv == 0);
    // 初始化条件变量
    rv = pthread_cond_init(&tp->not_empty, NULL);
    // 断言条件变量初始化成功
    assert(rv == 0);

    // 调整线程池中的线程数量
    tp->threads.resize(num_threads);
    // 创建线程
    for (size_t i = 0; i < num_threads; ++i) {
        // 创建线程，并将线程池对象作为参数传递给工作线程函数
        int rv = pthread_create(&tp->threads[i], NULL, &worker, tp);
        // 断言线程创建成功
        assert(rv == 0);
    }
}


/**
 * 将任务添加到线程池的任务队列中
 * @param tp 线程池对象指针
 * @param f 指向要执行的函数的指针
 * @param arg 传递给函数的参数
 */
void thread_pool_queue(TheadPool *tp, void (*f)(void *), void *arg) {
    // 创建一个新的工作任务
    Work w;
    // 设置工作任务的函数
    w.f = f;
    // 设置工作任务的参数
    w.arg = arg;

    // 加锁，保护任务队列的访问
    pthread_mutex_lock(&tp->mu);
    // 将新任务添加到任务队列中
    tp->queue.push_back(w);
    // 发送信号，通知等待的线程有新任务可用
    pthread_cond_signal(&tp->not_empty);
    // 解锁，允许其他线程访问任务队列
    pthread_mutex_unlock(&tp->mu);
}
