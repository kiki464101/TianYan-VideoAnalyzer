#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// 固定工作线程数的任务线程池：任务队列 + 条件变量唤醒
class ThreadPool
{
public:
    using Task = std::function<void()>;

    explicit ThreadPool(size_t threadCount = 4);
    ~ThreadPool();

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    // 启动 worker 线程
    void start();
    // 提交任务（队列满则阻塞或丢弃，由实现决定）
    bool enqueue(Task task);
    // 停止并回收所有线程
    void stop();

    size_t threadCount() const;
    size_t pendingTasks() const;

private:
    void workerLoop();

    std::vector<std::thread> m_workers;
    std::deque<Task>         m_tasks;
    std::mutex               m_mutex;
    std::condition_variable  m_cvNotEmpty;
    std::atomic<bool>        m_stopped{false};
    size_t                   m_threadCount;
};

#endif // THREADPOOL_H
