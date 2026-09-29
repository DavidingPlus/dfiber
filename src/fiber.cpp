#include "fiber.h"

#include "config.h"

#include <cassert>


// 正在运行的协程。
static thread_local Fiber *t_fiber = nullptr;
// 主协程。
static thread_local std::shared_ptr<Fiber> t_threadFiber = nullptr;
// 调度协程。
static thread_local Fiber *t_schedulerFiber = nullptr;

// 全局协程 ID 计数器。
static std::atomic<uint64_t> s_fiberId{0};
// 全局活跃协程数量计数器。
static std::atomic<uint64_t> s_fiberCount{0};


// 作用：创建主协程。设置状态，初始化上下文，并分配 ID。
Fiber::Fiber()
{
    SetThis(this);     // 在 getThis 中使用了无参的 Fiber 来构造 t_fiber。
    m_state = RUNNING; // 设置协程的状态为可运行。

    if (getcontext(&m_ctx))
    {
        std::cerr << "Fiber() failed\n";

        pthread_exit(nullptr);
    }

    m_id = ++s_fiberId; // 分配 id，协程 id 从 0 开始，用完加 1。
    ++s_fiberCount;     // 活跃的协程数量 +1。

    if (DFIBER_CONFIG_DEBUG) std::cout << "Fiber(): main id = " << m_id << std::endl;
}

// 作用：创建一个新协程，初始化回调函数，栈的大小和状态。分配栈空间，并通过 make 修改上下文当 set 或 swap 激活 ucontext_t，m_ctx 上下文时候会执行 make 第二个参数的函数。
Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool runInScheduler)
    : m_cb(cb), m_runInScheduler(runInScheduler)
{
    m_state = READY; // 初始化状态。

    // 分配协程栈空间。
    m_stackSize = stacksize ? stacksize : 128000;
    m_stack = malloc(m_stackSize);

    if (getcontext(&m_ctx))
    {
        std::cerr << "Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler) failed\n";

        pthread_exit(nullptr);
    }

    m_ctx.uc_link = nullptr; // 这里因为没有设置了后继所以在运行完 mainfunc 后协程退出，会调用一次 yield 返回主协程。
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stackSize;

    // 对工作协程而言，只要切换到这个协程，就会从 MainFunc 函数开始执行。
    makecontext(&m_ctx, &Fiber::MainFunc, 0);

    m_id = ++s_fiberId; // 分配 id，协程 id 从 0 开始，用完加 1。
    ++s_fiberCount;     // 活跃的协程数量 +1。

    if (DFIBER_CONFIG_DEBUG) std::cout << "Fiber(): child id = " << m_id << std::endl;
}

Fiber::~Fiber()
{
    --s_fiberCount; // 减少活跃协程计数器。

    if (m_stack)
    {
        free(m_stack);
        m_stack = nullptr;
    }

    if (DFIBER_CONFIG_DEBUG) std::cout << "~Fiber(): id = " << m_id << std::endl;
}

// 作用：重置协程的回调函数，并重新设置上下文，使用与将协程从 TERMINATE 状态重置 READY。
void Fiber::reset(std::function<void()> cb)
{
    assert(nullptr != m_stack && TERMINATE == m_state);

    m_state = READY;
    m_cb = cb;

    if (getcontext(&m_ctx))
    {
        std::cerr << "reset() failed\n";

        pthread_exit(nullptr);
    }

    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stackSize;

    makecontext(&m_ctx, &Fiber::MainFunc, 0);
}

// 作用：将协程的状态设置为 running，并恢复协程的执行。如果 m_runInScheduler 为 true，代表受调度协程管理，从调度协程切换到工作协程。否则代表受用户自己管理，只能从主协程切换到工作协程。
void Fiber::resume()
{
    assert(READY == m_state);

    m_state = RUNNING;

    // 这里的切换就相当于非对称协程函数那个当 a 执行完成后会将执行权交给 b。
    if (m_runInScheduler)
    {
        // 切换为目前工作的协程。
        SetThis(this);
        // 保存调度或者主 Fiber 现场，跳到工作 Fiber。
        // swapcontext(旧上下文, 新上下文);
        if (swapcontext(&(t_schedulerFiber->m_ctx), &m_ctx))
        {
            std::cerr << "resume() to t_schedulerFiber failed\n";

            pthread_exit(nullptr);
        }
    }
    else
    {
        SetThis(this);
        if (swapcontext(&(t_threadFiber->m_ctx), &m_ctx))
        {

            std::cerr << "resume() to t_schedulerFiber failed\n";

            pthread_exit(nullptr);
        }
    }
}

void Fiber::yield()
{
    assert(m_state == RUNNING || m_state == TERMINATE);

    if (TERMINATE != m_state) m_state = READY;

    if (m_runInScheduler)
    {
        SetThis(t_schedulerFiber);
        // 保存工作 Fiber 现场。恢复调度或者主 Fiber 现场。
        if (swapcontext(&m_ctx, &(t_schedulerFiber->m_ctx)))
        {
            std::cerr << "yield() to to t_schedulerFiber failed\n";

            pthread_exit(nullptr);
        }
    }
    else
    {
        SetThis(t_threadFiber.get());
        if (swapcontext(&m_ctx, &(t_threadFiber->m_ctx)))
        {
            std::cerr << "yield() to t_threadFiber failed\n";

            pthread_exit(nullptr);
        }
    }
}

void Fiber::SetThis(Fiber *f)
{
    t_fiber = f;
}

std::shared_ptr<Fiber> Fiber::GetThis()
{
    // 如果有正在运行的协程就直接返回。
    if (t_fiber) return t_fiber->shared_from_this();

    // 当前线程还没有 Fiber 环境，创建 Main Fiber。Main Fiber 作为当前线程的第一个协程，并默认作为调度协程。
    t_threadFiber = std::shared_ptr<Fiber>(new Fiber()); // 不能用 t_threadFiber = std::make_shared<Fiber>()，因为 Fiber() 的默认构造是私有的，不能从外部调用。
    // Fiber 类并不知道 Scheduler 类的存在，除非更改设计，否则在 Fiber 类的语义下，主协程默认为调度协程。如果存在 Scheduler 类，会手动调用 SetSchedulerFiber() 函数设置。
    t_schedulerFiber = t_threadFiber.get();

    assert(t_fiber == t_threadFiber.get()); // 用于判断，t_fiber 是否等于 main_fiber。是继续执行，否则程序终止。


    return t_fiber->shared_from_this();
}

void Fiber::SetSchedulerFiber(Fiber *f)
{
    t_schedulerFiber = f;
}

uint64_t Fiber::GetFiberId()
{
    if (t_fiber) return t_fiber->getId();

    return (uint64_t)-1; // 返回 -1，并且是 (uint64_t)-1 那就会转换成 UINT64_MAX，用来表示错误的情况。
}

void Fiber::MainFunc()
{
    std::shared_ptr<Fiber> cur = GetThis(); // GetThis() 的 shared_from_this() ⽅法让引⽤计数加 1。
    assert(nullptr != cur);

    cur->m_cb();
    cur->m_cb = nullptr;
    cur->m_state = TERMINATE;

    // 运行完毕 -> 让出执行权。
    // std::shared_ptr 的 get() 函数只是拿到对象地址，不增加引用计数。
    auto rawPtr = cur.get();
    // 这里只释放 rawPtr 这个临时 shared_ptr，并不会销毁对象，因为 Scheduler 的任务列表里还有一个 shared_ptr 指向这个 Fiber。这个协程对象可能会被其他线程调度，因此不能直接销毁对象。
    // std::shared_ptr 的 reset() 函数做了两件事情，引用计数减 1，自己变成空指针，因此对象不一定销毁。
    cur.reset();

    // 提前释放 MainFunc 中持有的 std::shared_ptr，避免协程 yield 后不再返回，导致引用计数无法减少。
    // 由于 Scheduler 仍持有该 Fiber，因此对象不会立即析构，使用提前保存的裸指针完成最后一次 yield 即可。
    rawPtr->yield();
}
