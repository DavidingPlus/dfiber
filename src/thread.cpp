#include "thread.h"

#include <pthread.h>
#include <unistd.h>


DFIBER_NAMESPACE_BEGIN(dfiber)


// 线程信息。
// static 表示变量的生命周期持续到程序结束时才销毁。
// thread_local 表示变量是线程局部的，即每个访问该变量的线程都会拥有一个独立副本。例如每个线程都会独立拥有一个 Thread 指针和当前线程名称，多个线程的副本互不干扰。
static thread_local Thread *t_thread = nullptr;           // 当前线程的 Thread 对象指针。
static thread_local std::string t_threadName = "UNKNOWN"; // 当前线程的名称。


Thread::Thread(std::function<void()> cb, const std::string &name)
    : m_cb(cb), m_name(name)
{
    int res = pthread_create(&m_thread, nullptr, &Thread::run, this); // 这里需要注意的是 this 是传递给 run 函数进行转换的。
    if (res)
    {
        std::cerr << "pthread_create thread failed, res=" << res << " name=" << name;
        throw std::logic_error("pthread_create error");
    }

    // 等待线程函数完成初始化。
    m_semaphore.wait();
}

Thread::~Thread()
{
    if (m_thread)
    {
        pthread_detach(m_thread);

        m_thread = 0;
    }
}

void Thread::join()
{
    if (m_thread)
    {
        int res = pthread_join(m_thread, nullptr);
        if (res)
        {
            std::cerr << "pthread_join failed, res = " << res << ", name = " << m_name << std::endl;
            throw std::logic_error("pthread_join error");
        }

        m_thread = 0;
    }
}


pid_t Thread::GetThreadId()
{
    /*
     * Linux / POSIX 进程与线程 ID 说明：
     *
     * 1. 进程 ID（PID）：
     *    - 类型：pid_t
     *    - 获取方式：getpid()
     *    - 含义：进程在系统中的唯一标识（在 Linux 中也称 TGID，线程组 ID）
     *
     * 2. 线程 ID（TID）分两种概念：
     *
     *    (1) POSIX 线程 ID（用户态抽象）：
     *        - 类型：pthread_t
     *        - 获取方式：pthread_self()
     *        - 含义：pthread 库使用的线程句柄（不保证是整数，也不等同于内核线程 ID）
     *        - 特点：仅用于 pthread API 管理线程，属于用户态抽象
     *
     *    (2) Linux 内核线程 ID（真实调度实体）：
     *        - 类型：pid_t
     *        - 获取方式：gettid() 或 syscall(SYS_gettid)
     *        - 含义：内核调度层面的线程唯一 ID
     *        - 特点：每个线程都有独立 TID，但同一进程内线程共享同一个 PID（TGID）
     *
     * 3. 关键关系：
     *    - getpid()  → 返回线程组 ID（TGID，即进程 ID）
     *    - gettid()  → 返回内核线程 ID（TID）
     *    - pthread_t → 用户态线程句柄（不等价于 PID/TID）
     */

    return gettid(); // 等价于 syscall(SYS_gettid) 是一个系统调用，用于获取当前线程的唯一 ID。SYS_gettid 是 Linux 特定的系统调用编号，用来获取线程 ID(TID)。pid_t 是一个数据类型，用于表示进程 ID 或线程 ID。
}

Thread *Thread::GetThis()
{
    return t_thread;
}

const std::string &Thread::GetName()
{
    return t_threadName;
}

void Thread::SetName(const std::string &name)
{
    if (t_thread) t_thread->m_name = name;

    t_threadName = name;
}

void *Thread::run(void *arg)
{
    Thread *thread = reinterpret_cast<Thread *>(arg);

    t_thread = thread;
    t_threadName = thread->m_name;

    thread->m_id = GetThreadId();
    // pthread_self() 获取当前线程的 ID，设置 m_name 是前 15 个字节取，目的就是设置线程的名字方便调试。
    // 存在可能被问的问题为什么是 (0, 15)，由于操作系统对线程名称长度的限制决定的，在 linux 中线程最大的名字只能是 15，后面还有一个 \0 总共 16。
    pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());

    // 在线程内部创建一个“本地任务容器”。用来存放真正要执行的函数（任务），和 thread->m_cb 解耦，避免直接在 m_cb 上操作。
    std::function<void()> cb;

    // 等价于：
    // cb = std::move(thread->m_cb);
    // thread->m_cb = nullptr;
    // 本质是任务所有权转移，把“线程对象里的任务”转移到当前运行函数里执行。swap 可以减少 m_cb 中只能指针的引用计数。
    // Thread 对象通常在主线程创建，真正执行是在新线程 run() 里，必须把任务“交给新线程”。
    cb.swap(thread->m_cb);

    // 初始化完成，这里确保了主线程创建出来一个工作线程，提供给协程使用，否则可能出现协程在未出初始化的线程上使用。
    // 通知“线程已经启动完成”，让“创建线程的人”知道：这个线程已经完成初始化，可以安全使用了。
    thread->m_semaphore.signal();

    // 真正执行函数的地方。
    try
    {
        cb();
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << std::endl;
    }


    return 0;
}


DFIBER_NAMESPACE_END
