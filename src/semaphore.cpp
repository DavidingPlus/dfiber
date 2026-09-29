#include "semaphore.h"


DFIBER_NAMESPACE_BEGIN(dfiber)


// P 操作。
void Semaphore::wait()
{
    std::unique_lock<std::mutex> lock(m_mtx); // 这里不使用 lock_guard 的原因是不允许手动解锁，是因为条件变量 m_cv.wait() 中会将锁释放，如果使用 lock_guard 函数没有结束的话释放不了锁。

    // notify_one() 的目的是只唤醒一个线程，但由于操作系统的虚假唤醒机制，或者在多线程竞争环境下，唤醒的线程在获取锁之前，业务条件可能已经被其他线程改变了（信号窃取）。
    // 因此，我们不能假设“醒来就等同于”条件满足。必须使用 while 循环重新检查条件：如果条件不满足，证明这次唤醒是虚假的“或者是迟到的”，线程必须再次进入等待状态。
    while (0 == m_count)
    {
        m_cv.wait(lock); // wait for signals
    }
    --m_count;
}

// V 操作，这里是负责给 m_count++，然后通知 wait 唤醒等待的线程。
void Semaphore::signal()
{
    std::unique_lock<std::mutex> lock(m_mtx);
    ++m_count;
    m_cv.notify_one(); // signal，要注意这里的 one 指的不是只有一个可能是多个线程。
}


DFIBER_NAMESPACE_END
