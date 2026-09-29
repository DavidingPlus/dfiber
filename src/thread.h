#ifndef _DFIBER_THREAD_H_
#define _DFIBER_THREAD_H_

#include <iostream>
#include <thread>
#include <functional>
#include <string>

#include "semaphore.h"


class Thread
{

public:

    Thread(std::function<void()> cb, const std::string &name);

    virtual ~Thread();

    pid_t getId() const { return m_id; }

    const std::string &getName() const { return m_name; }

    void join();


    static pid_t GetThreadId(); // 获取系统分配的线程 id。

    static Thread *GetThis(); // 获取当前所在线程。

    static const std::string &GetName(); // 获取当前线程的名字。

    static void SetName(const std::string &name); // 设置当前线程的名字。


private:

    static void *run(void *arg); // 线程函数。


    pid_t m_id = -1; // 进程的 id。

    pthread_t m_thread = 0; // 线程。

    std::function<void()> m_cb; // 线程需要运行的函数。

    std::string m_name; // 线程的 name。

    // 引入信号量的类来完成线程的同步创建，而不是线程间的业务同步。
    // pthread_create() 仅表示线程已创建成功，并不能保证新线程已经开始执行。因此构造函数需要等待新线程完成初始化（如设置线程 ID、线程名称、thread_local 等信息）后再返回，避免调用方在构造完成后立即访问这些成员时发生竞争条件。新线程初始化完成后调用 signal()，构造函数通过 wait() 等待，从而保证 Thread 对象一旦构造完成，就已经处于可安全使用的状态。
    Semaphore m_semaphore;
};


#endif
