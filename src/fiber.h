#ifndef _DFIBER_FIBER_H_
#define _DFIBER_FIBER_H_

#include "globalmacros.h"

#include <iostream>
#include <memory>
#include <atomic>
#include <functional>
#include <mutex>

#include <ucontext.h>
#include <unistd.h>


DFIBER_NAMESPACE_BEGIN(dfiber)


// std::enable_shared_from_this<T> 允许一个对象在成员函数内部安全获得指向自己的 std::shared_ptr。
class Fiber : public std::enable_shared_from_this<Fiber>
{

public:

    // 定义协程的状态，属于协程的上下文切换的时候，需要被保存。
    enum State
    {
        READY = 0,
        RUNNING,
        TERMINATE
    };


private:

    // Fiber() 是私有的，只能被 GetThis() 方法调用，用于创建主协程。
    Fiber();


public:

    // 用于创建指定回调函数、栈大小和 runInScheduler 本协程是否参与调度器调度，默认为 true（这个概念是和调度器 scheduler 连接在一起的，结合调度器理解）。stacksize 传入 0 代表自动分配栈空间大小。
    Fiber(std::function<void()> cb, size_t stacksize = 0, bool runInScheduler = true);

    virtual ~Fiber();


public:

    // 重置协程状态和⼊⼝函数，复⽤栈空间，不重新创建栈。
    void reset(std::function<void()> cb);

    // 恢复协程执行。
    void resume();

    // 将执行全还给调度协程或者主协程。
    void yield();

    // 获取唯一标识。
    uint64_t getId() const { return m_id; }

    // 获取协程状态。
    State getState() const { return m_state; }


public:

    // 设置当前运行的协程。
    static void SetThis(Fiber *f);

    // 获取当前运行的协程的 shared_ptr 实例。如果没有，说明线程还没有 Fiber 环境，创建 Main Fiber。Main Fiber 作为当前线程的第一个协程，并默认作为调度协程。
    static std::shared_ptr<Fiber> GetThis();

    // 设置调度协程，默认主协程。
    static void SetSchedulerFiber(Fiber *f);

    // 获取当前运行的协程的 ID。
    static uint64_t GetFiberId();

    // 协程的主函数，入口点。
    static void MainFunc();


public:

    std::mutex m_mutex;


private:

    // 协程唯一标识符。
    uint64_t m_id = 0;

    // 栈的大小。
    uint32_t m_stackSize = 0;

    // 协程的初始状态是 ready。
    State m_state = READY;

    // 协程的上下文。
    ucontext_t m_ctx;

    // 协程栈的指针。
    void *m_stack = nullptr;

    // 协程的回调函数。
    std::function<void()> m_cb;

    // 用于标记该协程是否受调度器管理。
    bool m_runInScheduler;
};


DFIBER_NAMESPACE_END


#endif
