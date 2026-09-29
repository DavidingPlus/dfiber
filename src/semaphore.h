#ifndef _DFIBER_SEMAPHORE_H_
#define _DFIBER_SEMAPHORE_H_

#include <mutex>
#include <condition_variable>


// 使用锁和条件变量来实现信号量。用于线程方法间的同步。
class Semaphore
{

public:

    // 信号量初始化为 0。
    explicit Semaphore(int count = 0) : m_count(count) {}

    virtual ~Semaphore() = default;

    // P 操作
    void wait();

    // V 操作。
    void signal();


private:

    std::mutex m_mtx;

    std::condition_variable m_cv;

    int m_count;
};


#endif
