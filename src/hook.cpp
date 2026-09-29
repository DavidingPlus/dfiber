#include "hook.h"

#include "fdmanager.h"
#include "iomanager.h"

#include "config.h"

#include <iostream>
#include <cstring>
#include <cstdarg>

#include <dlfcn.h>


// 宏 HOOK_FUN(XX) 是一个宏展开机制，通过将 XX 依次应用于宏定义中的每一个函数名称来生成一系列代码。这种方式可以有效减少重复代码，提高代码的可读性和维护性。
#define HOOK_FUN(XX) \
    XX(sleep)        \
    XX(usleep)       \
    XX(nanosleep)    \
    XX(socket)       \
    XX(connect)      \
    XX(accept)       \
    XX(read)         \
    XX(readv)        \
    XX(recv)         \
    XX(recvfrom)     \
    XX(recvmsg)      \
    XX(write)        \
    XX(writev)       \
    XX(send)         \
    XX(sendto)       \
    XX(sendmsg)      \
    XX(close)        \
    XX(fcntl)        \
    XX(ioctl)        \
    XX(getsockopt)   \
    XX(setsockopt)


DFIBER_NAMESPACE_BEGIN(dfiber)


// 使用线程局部变量，每个线程都会判断一下是否启用了钩子。
// 表示当前线程是否启用了钩子功能。初始值为 false，即钩子功能默认关闭。
static thread_local bool tHookEnable = false;


bool isHookEnable()
{
    return tHookEnable;
}

void setHookEnable(bool flag)
{
    tHookEnable = flag;
}

void hookInit()
{
    // 通过一个静态变量来确保 hookInit() 只初始化一次，防止重复初始化。
    static bool isInited = false;
    if (isInited) return;

    isInited = true;

// 由于 hook 库通过 LD_PRELOAD 机制被动态链接器优先加载，例如当程序调用 sleep() 时，动态链接器会优先解析到当前 hook 库中的 sleep 实现，而不是 libc.so 中原始的 sleep 函数。
// 但是在 hook 函数内部，我们仍然需要调用真正的 libc 实现，否则直接调用 sleep() 会再次进入自己的 hook 函数，导致无限递归。
// 因此需要使用 dlsym() 获取原始函数地址。RTLD_NEXT 表示从当前动态库之后继续搜索符号，跳过当前 hook 库，从后续加载的共享库中查找真实实现。
// 例如：
//
//      调用 sleep()
//            |
//            v
//      libhook.so:sleep()       <-- 当前 hook 函数
//            |
//            | dlsym(RTLD_NEXT, "sleep")
//            v
//      libc.so:sleep()          <-- 原始系统调用
//
// 最终 sleep_f 保存的就是 libc 中真实 sleep 函数的地址，hook 函数可以通过 sleep_f 间接调用原始实现。

// 使用 X-Macro 技巧批量初始化 hook 函数对应的原始函数指针。
//
// HOOK_FUN(XX) 会遍历所有需要 hook 的函数，并将函数名传递给 XX 宏。
// 例如：
//
// XX(sleep)
//
// 会展开为：
//
// sleep_f = (sleep_fun)dlsym(RTLD_NEXT, "sleep");
//
// 其中：
// - name##_f：## 用于拼接变量名，例如 sleep -> sleep_f。
// - name##_fun：拼接函数指针类型，例如 sleep -> sleep_fun。
// - #name：将函数名转换为字符串，例如 sleep -> "sleep"。
//
// 最终 HOOK_FUN(XX) 会展开成所有 hook 函数的 dlsym 初始化代码，避免为每个函数重复手写初始化逻辑。
#define XX(name)                                    \
    name##_f = (name##_fun)dlsym(RTLD_NEXT, #name); \
    if (DFIBER_CONFIG_DEBUG)                        \
        std::cout << #name << ": " << (void *)name##_f << std::endl;

    HOOK_FUN(XX)
#undef XX
}


struct HookIniter
{
    // 钩子函数。初始化 hook，让原始调用绑定到宏展开的函数指针中。
    HookIniter() { hookInit(); }
};

// 定义了一个静态的 HookIniter 实例。由于静态变量的初始化发生在 main() 函数之前，所以 hookInit() 会在程序开始时被调用，从而初始化钩子函数。
static HookIniter sHookIniter;


// 用于跟踪定时器的状态。它有一个 cancelled 成员变量，通常用于表示定时器是否已经被取消。
struct TimerInfo
{
    // 表示定时器是否已经被取消。
    int cancelled = 0;
};


/**
 * @brief 将阻塞式 IO 操作转换为协程异步 IO。
 *
 * doIo() 是对阻塞式 IO 操作 read/write/recv/send/connect 等可能阻塞的系统调用的统一封装。它根据当前文件描述符状态决定是否直接调用原始系统调用，或者将可能阻塞的 IO 操作转换为协程异步等待。当 IO 暂时无法完成时，doIo 会将当前协程挂起，并通过 IOManager 等待对应事件发生；如果设置了超时时间，则结合定时器机制处理超时情况。最终，doIo 将传统阻塞 IO 转换为基于事件驱动的协程 IO，避免阻塞线程，提高并发处理能力。
 *
 * 整体执行流程：
 *
 *  1. 调用原始系统 IO 函数（libc 函数）
 *
 *                    +----------------+
 *                    | fun(fd, args)  |
 *                    +----------------+
 *                            |
 *                            v
 *                  +-------------------+
 *                  | 返回值 >= 0 ?     |
 *                  +-------------------+
 *                     |             |
 *                    是             否
 *                     |             |
 *                     v             v
 *              返回 IO 结果     errno == EINTR ?
 *                                   |
 *                         +---------+---------+
 *                         |                   |
 *                        是                   否
 *                         |                   |
 *                         v                   v
 *                      retry           errno == EAGAIN ?
 *                                             |
 *                                  +----------+----------+
 *                                  |                     |
 *                                 否                     是
 *                                  |                     |
 *                                  v                     v
 *                             返回错误          当前 IO 暂不可用
 *
 *
 *  2. EAGAIN 处理流程：
 *
 *             errno == EAGAIN
 *                    |
 *                    v
 *        +-------------------------+
 *        | 获取 IOManager          |
 *        +-------------------------+
 *                    |
 *                    v
 *        是否设置 IO 超时时间？
 *                    |
 *          +---------+---------+
 *          |                   |
 *         是                   否
 *          |                   |
 *          v                   |
 *     添加条件定时器              |
 *          |                   |
 *          +---------+---------+
 *                    |
 *                    v
 *        IOManager::addEvent(fd,event)
 *                    |
 *          +---------+---------+
 *          |                   |
 *         失败                 成功
 *          |                   |
 *          v                   v
 *     取消 timer          当前 Fiber yield()
 *     返回错误                   |
 *                              |
 *                              v
 *                 +------------+------------+
 *                 |                         |
 *             IO事件触发                 Timer超时
 *                 |                         |
 *                 v                         v
 *          IOManager恢复Fiber        timer callback
 *                 |                         |
 *                 |                  设置 ETIMEDOUT
 *                 |                  cancelEvent()
 *                 |                         |
 *                 +------------+------------+
 *                              |
 *                              v
 *                    Fiber 从 yield 恢复
 *                              |
 *                              v
 *                       取消超时 timer
 *                              |
 *                 +------------+------------+
 *                 |                         |
 *             超时结束                  IO事件完成
 *                 |                         |
 *                 v                         v
 *          errno=ETIMEDOUT              goto retry
 *          return -1                       |
 *                                           v
 *                                重新调用 fun(fd,args...)
 *
 */
/**
 * @param fd 需要进行 IO 操作的文件描述符。
 * @param fun 原始系统调用函数指针，例如 read_f、write_f，用于在满足条件时调用真实 libc 函数。
 * @param hookFunName 当前 hook 函数名称，仅用于调试输出和错误定位。
 * @param event 需要监听的 IO 事件类型，例如 IOManager::READ 或 IOManager::Event:: WRITE。当 IO 暂时不可用时，IOManager 根据该事件等待 fd 状态变化。
 * @param timeoutSo 超时选项对应的 socket 参数，例如 SO_RCVTIMEO 或 SO_SNDTIMEO，用于从 FdCtx 中获取对应 IO 超时时间。
 * @param args 传递给原始系统调用的参数。使用完美转发保持参数原始类型，支持不同 IO 函数的参数形式。
 */
template <typename OriginFun, typename... Args>
static ssize_t doIo(int fd, OriginFun fun, const char *hookFunName, uint32_t event, int timeoutSo, Args &&...args)
{
    // 如果全局钩子功能未启用，则直接调用原始的 I/O 函数。
    if (!tHookEnable) return fun(fd, std::forward<Args>(args)...);

    // 获取与文件描述符 fd 相关联的上下文 ctx。
    std::shared_ptr<FdCtx> ctx = FdMgr::GetInstance()->get(fd);
    // 如果上下文不存在，则直接调用原始的 I/O 函数。
    if (!ctx) return fun(fd, std::forward<Args>(args)...);
    // 如果文件描述符已经关闭，设置 errno 为 EBADF 并返回 -1。
    if (ctx->isClosed())
    {
        // Bad file number，表示文件描述符无效或已经关闭。
        errno = EBADF;
        return -1;
    }
    // 如果文件描述符不是一个 socket 或者用户设置了非阻塞模式，则直接调用原始的 I/O 操作函数。
    if (!ctx->isSocket() || ctx->getUserNonblock()) return fun(fd, std::forward<Args>(args)...);

    // 获取超时设置并初始化 TimerInfo 结构体，用于后续的超时管理和取消操作。
    uint64_t timeout = ctx->getTimeout(timeoutSo);
    std::shared_ptr<TimerInfo> tinfo(new TimerInfo);

retry:
    // 执行原始 I/O 函数。如果正确执行，则会按照原始 I/O 函数正常返回退出函数。doIo() hook 的是 I/O 操作失败时需要额外做的事情。
    ssize_t n = fun(fd, std::forward<Args>(args)...);

    // 如果 I/O 系统调用失败，并且失败原因是被信号打断（EINTR），那么重新执行一次系统调用，直到不是因为 EINTR 的原因返回。
    while (-1 == n && EINTR == errno) n = fun(fd, std::forward<Args>(args)...);

    // 如果 I/O 操作因为资源暂时不可用（EAGAIN）而失败，函数会添加一个事件监听器来等待资源可用。同时，如果有超时设置，还会启动一个条件计时器来取消事件。
    if (-1 == n && EAGAIN == errno)
    {
        IOManager *iom = IOManager::GetThis();
        std::shared_ptr<Timer> timer;
        std::weak_ptr<TimerInfo> winfo(tinfo);

        // 1. 如果执行的 read 等函数在 FdManager 管理的 FdCtx 中 fd 设置了超时时间，就会走到这里。添加 addconditionTimer() 事件。
        if (static_cast<uint64_t>(-1) != timeout) // 超时时间默认是 -1。
        {
            timer = iom->addConditionTimer(
                timeout,
                [winfo, fd, iom, event]()
                {
                    auto t = winfo.lock();
                    // 如果 TimerInfo 对象已被释放（!t），或者操作已被取消（t->cancelled 非 0），则直接返回。
                    if (!t || t->cancelled) return;

                    // 超时时间到达，记录这次 IO 等待结束的原因是“超时”。
                    t->cancelled = ETIMEDOUT;
                    // 取消该文件描述符上的事件，并立即触发一次事件。
                    iom->cancelEvent(fd, static_cast<IOManager::Event>(event));
                },
                winfo);
        }

        // 2. 将 fd（文件描述符）和 event（要监听的事件，如读或写事件）添加到 IOManager 中进行管理。IOManager 会监听这个文件描述符上的事件，当事件触发时，它会调度相应的协程来处理这个事件。
        int res = iom->addEvent(fd, static_cast<IOManager::Event>(event));
        // 如果 res 为 -1，说明 addEvent 失败。取消之前设置的定时器，避免误触发。
        if (-1 == res)
        {
            if (DFIBER_CONFIG_DEBUG) std::cout << hookFunName << " addEvent(" << fd << ", " << event << ") failed";

            if (timer) timer->cancel();


            return -1;
        }
        // 如果 addEvent 成功（res 为 0），当前协程会调用 yield() 函数，将自己挂起，等待事件的触发。
        else
        {
            Fiber::GetThis()->yield();

            // 3. 当协程被恢复时（例如，事件触发后），它会继续执行 yield() 之后的代码。如果之前设置了定时器（timer 不为 nullptr），则需取消该定时器。因为当前 Fiber 已经因为 I/O 事件或超时事件恢复执行，说明本次 I/O 等待已经结束。如果之前创建了超时定时器，需要将其取消，避免 I/O 已经完成后定时器仍然触发，干扰后续的 I/O 操作或重复处理事件。
            if (timer) timer->cancel();

            // 检查当前 Fiber 是否因为 I/O 超时而被唤醒。如果前面设置的条件定时器触发，会将 tinfo->cancelled 标记为 ETIMEDOUT，并通过 cancelEvent() 唤醒 Fiber，已经执行过 I/O 事件了，此时直接返回超时错误，不再重新尝试 I/O。
            if (ETIMEDOUT == tinfo->cancelled)
            {
                errno = tinfo->cancelled;
                return -1;
            }
            // 如果没有超时，则 retry 正常执行原始 I/O 函数，然后退出函数。
            else
            {
                goto retry;
            }
        }
    }


    return n;
}


DFIBER_NAMESPACE_END


extern "C"
{

// 使用 X-Macro 技巧批量定义所有 hook 函数对应的原始函数指针，并初始化为空。
// 例如：
//
// XX(sleep)
//
// 会展开为：
//
// sleep_fun sleep_f = nullptr;
//
// 最终 HOOK_FUN(XX) 会展开成：
//
// sleep_fun sleep_f = nullptr;
// usleep_fun usleep_f = nullptr;
// socket_fun socket_f = nullptr;
// connect_fun connect_f = nullptr;
// ...
//
// 后续在 hook 初始化阶段，再通过 dlsym(RTLD_NEXT, "...") 为这些函数指针赋值，使其指向 libc 中对应的原始系统调用实现。
#define XX(name) name##_fun name##_f = nullptr;
    HOOK_FUN(XX)
#undef XX


    // 实现了一个协程版本的 sleep，通过钩子机制拦截 sleep 的调用，并将其改为使用协程来实现非阻塞的休眠。
    unsigned int sleep(unsigned int seconds)
    {
        // 如果钩子未启用，则调用原始的系统调用。
        if (!dfiber::tHookEnable) return sleep_f(seconds);

        // 获取当前正在执行的协程（Fiber），并将其保存到 fiber 变量中。
        std::shared_ptr<dfiber::Fiber> fiber = dfiber::Fiber::GetThis();
        // 获取当前线程的调度器对象（IOManager）指针，用于添加定时器任务。
        dfiber::IOManager *iom = dfiber::IOManager::GetThis();

        iom->addTimer(seconds * 1000, [fiber, iom]()
                      { iom->scheduleLock(fiber); });
        // 挂起当前协程的执行，将控制权交还给调度器。
        fiber->yield();


        return 0;
    }

    int usleep(useconds_t usec)
    {
        if (!dfiber::tHookEnable) return usleep_f(usec);

        std::shared_ptr<dfiber::Fiber> fiber = dfiber::Fiber::GetThis();
        dfiber::IOManager *iom = dfiber::IOManager::GetThis();

        // useconds_t 一个无符号整数类型，通常用于表示微秒数。在这个函数中，usec 表示延时的微秒数，将其转换为毫秒数 (usec/1000) 后用于定时器。
        iom->addTimer(usec / 1000, [fiber, iom]()
                      { iom->scheduleLock(fiber); });
        fiber->yield();


        return 0;
    }

    int nanosleep(const struct timespec *req, struct timespec *rem)
    {
        if (!dfiber::tHookEnable) return nanosleep_f(req, rem);

        // 将 tv_sec 转换为毫秒，并将 tv_nsec 转换为毫秒，然后两者相加得到总的超时毫秒数。所以从这里看出实现的也是一个毫秒级的操作。
        uint64_t timeoutMs = req->tv_sec * 1000 + req->tv_nsec / 1000 / 1000;

        std::shared_ptr<dfiber::Fiber> fiber = dfiber::Fiber::GetThis();
        dfiber::IOManager *iom = dfiber::IOManager::GetThis();

        iom->addTimer(timeoutMs, [fiber, iom]()
                      { iom->scheduleLock(fiber); });
        fiber->yield();


        return 0;
    }

    int socket(int domain, int type, int protocol)
    {
        if (!dfiber::tHookEnable) return socket_f(domain, type, protocol);

        // 如果钩子启用了，则通过调用原始的 socket 函数创建套接字，并将返回的文件描述符存储在 fd 变量中。
        int fd = socket_f(domain, type, protocol);
        // fd 是无效的情况。
        if (-1 == fd)
        {
            std::cerr << "socket() failed:" << strerror(errno) << '\n';
            return fd;
        }

        // 如果 socket 创建成功，使用 get() 函数将其加入 FdManager 的文件描述符管理类来进行管理。具体判断是否在其管理的文件描述符中，如果不在扩展存储文件描述数组大小，并且利用 FdCtx 进行初始化判断是是不是套接字，是不是系统非阻塞模式。
        dfiber::FdMgr::GetInstance()->get(fd, true);


        return fd;
    }
}


DFIBER_NAMESPACE_BEGIN(dfiber)


static uint64_t sConnectTimeout = -1;


// 在连接超时情况下处理非阻塞套接字连接的实现。它首先尝试使用钩子功能来捕获并管理连接请求的行为，然后使用 IOManager 和 Timer 来管理超时机制，逻辑实现和 doIo 类似。
// 注意：如果没有启用 hook 或者不是一个套接字、用户启用了非阻塞。都会调用 connect 系统调用，因为 connectWithTimeout 本身就在 connect 系统调用基础上实现的。
static int connectWithTimeout(int fd, const struct sockaddr *addr, socklen_t addrlen, uint64_t timeoutMs)
{
    if (!tHookEnable) return connect_f(fd, addr, addrlen);

    // 获取文件描述符 fd 的上下文信息 FdCtx。
    std::shared_ptr<FdCtx> ctx = FdMgr::GetInstance()->get(fd);
    // 如果上下文不存在，则直接回落到原始 connect 保持系统 errno 语义。
    if (!ctx) return connect_f(fd, addr, addrlen);
    // 检查文件描述符上下文是否已关闭。
    if (ctx->isClosed())
    {
        // EBADF 表示一个无效的文件描述符。
        errno = EBADF;
        return -1;
    }
    // 如果文件描述符不是一个 socket 或者用户设置了非阻塞模式，则直接调用原始的 I/O 操作函数。
    if (!ctx->isSocket() || ctx->getUserNonblock()) return connect_f(fd, addr, addrlen);

    // 第一次尝试调用非阻塞 connect。connect 有三种情况：
    // 1. 返回 0：连接立即成功，直接返回。
    // 2. 返回 -1 且 errno != EINPROGRESS：连接立即失败，直接返回错误。
    // 3. 返回 -1 且 errno == EINPROGRESS：连接正在后台建立，需要通过 IOManager 等待连接完成。
    int n = connect_f(fd, addr, addrlen);
    // 立即成功，直接返回。
    if (0 == n) return 0;
    // 这里是用了下面分支的取反条件判断的，只有 EINPROGRESS 表示非阻塞连接正在进行。其他情况（例如连接被拒绝、网络不可达等）都已经得到最终结果，直接返回。
    if (!(-1 == n && EINPROGRESS == errno)) return n;

    // 非阻塞 connect 正在进行。交给 IOManager 等待 WRITE 事件。下面逻辑和 doIo() 类似。
    IOManager *iom = IOManager::GetThis();           // 获取当前线程的 IOManager 实例。
    std::shared_ptr<Timer> timer;                    // 声明一个定时器对象。
    std::shared_ptr<TimerInfo> tinfo(new TimerInfo); // 创建追踪定时器是否取消的对象。
    std::weak_ptr<TimerInfo> winfo(tinfo);           // 判断追踪定时器对象是否存在。

    // 超时时间先触发和写事件先触发会导致什么问题？
    // 1. 超时：如果是超时事件比写资源先触发，那么调度任务肯定执行到定时回调就相当于把 addevent 的写事件处理了，然后给 tinfo->cancelled 标记超时，执行完后子协程让出执行权给调度协程，调度协程从 yield 的地方继续，删除定时器，并且 tinfo->cancelled 如果有值就是非 0 的情况，就将其错误给 errno，然后退出。
    // 2. 写事件：如果是写事件触发，会直接执行到 yield 暂停的后面，并且 cancel 会取消上面设置的定时器，所以不可能发生再次调用定时器的回调这个过程，接下来就正常往下走后正确返回就行了。

    // 检查是否设置了超时时间。如果 timeoutMs 不等于 -1，则创建一个定时器。
    if (static_cast<uint64_t>(-1) != timeoutMs)
    {
        // 添加一个定时器，当超时时间到达时，取消事件监听并设置 cancelled 状态。
        timer = iom->addConditionTimer(
            timeoutMs, [winfo, fd, iom]()
            {
                    auto t = winfo.lock();
                    if (!t || t->cancelled) return;

                    t->cancelled = ETIMEDOUT;
                    // 将指定的 fd 的事件触发将事件处理。
                    iom->cancelEvent(fd, IOManager::Event::WRITE); },
            winfo);
    }

    // 为文件描述符 fd 添加一个写事件监听器。这样的目的是为了上面的回调函数处理指定文件描述符。
    int res = iom->addEvent(fd, IOManager::Event::WRITE);
    // 添加事件成功。
    if (0 == res)
    {
        Fiber::GetThis()->yield();

        // 如果有定时器，取消定时器。
        if (timer) timer->cancel();

        // 发生超时错误或者用户取消。
        if (tinfo->cancelled)
        {
            // 赋值给 errno 通过其查看具体错误原因。
            errno = tinfo->cancelled;
            return -1;
        }
    }
    // 添加事件失败。
    else
    {
        if (timer) timer->cancel();

        std::cerr << "connect addEvent(" << fd << ", WRITE) error";
    }

    // 检查连接是否建立成功。
    int error = 0;
    socklen_t len = sizeof(int);
    // 通过 getsockopt() 检查套接字实际错误状态来判断是否成功或失败。
    if (-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len))
    {
        return -1;
    }
    // 如果没有错误，返回 0 表示连接成功。
    if (!error)
    {
        return 0;
    }
    // 如果有错误，设置 errno 并返回错误。
    else
    {
        errno = error;
        return -1;
    }
}


DFIBER_NAMESPACE_END


extern "C"
{

    int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
    {
        return dfiber::connectWithTimeout(sockfd, addr, addrlen, dfiber::sConnectTimeout);
    }

    int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
    {
        int fd = dfiber::doIo(sockfd, accept_f, "accept", static_cast<uint32_t>(dfiber::IOManager::Event::READ), SO_RCVTIMEO, addr, addrlen);
        // 同理添加到文件描述符管理器 FdManager 中。
        if (fd >= 0) dfiber::FdMgr::GetInstance()->get(fd, true);


        return fd;
    }

    ssize_t read(int fd, void *buf, size_t count)
    {
        return dfiber::doIo(fd, read_f, "read", static_cast<uint32_t>(dfiber::IOManager::Event::READ), SO_RCVTIMEO, buf, count);
    }

    ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
    {
        return dfiber::doIo(fd, readv_f, "readv", static_cast<uint32_t>(dfiber::IOManager::Event::READ), SO_RCVTIMEO, iov, iovcnt);
    }

    ssize_t recv(int sockfd, void *buf, size_t len, int flags)
    {
        return dfiber::doIo(sockfd, recv_f, "recv", static_cast<uint32_t>(dfiber::IOManager::Event::READ), SO_RCVTIMEO, buf, len, flags);
    }

    ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
    {
        return dfiber::doIo(sockfd, recvfrom_f, "recvfrom", static_cast<uint32_t>(dfiber::IOManager::Event::READ), SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);
    }

    ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
    {
        return dfiber::doIo(sockfd, recvmsg_f, "recvmsg", static_cast<uint32_t>(dfiber::IOManager::Event::READ), SO_RCVTIMEO, msg, flags);
    }

    ssize_t write(int fd, const void *buf, size_t count)
    {
        return dfiber::doIo(fd, write_f, "write", static_cast<uint32_t>(dfiber::IOManager::Event::WRITE), SO_SNDTIMEO, buf, count);
    }

    ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
    {
        return dfiber::doIo(fd, writev_f, "writev", static_cast<uint32_t>(dfiber::IOManager::Event::WRITE), SO_SNDTIMEO, iov, iovcnt);
    }

    ssize_t send(int sockfd, const void *buf, size_t len, int flags)
    {
        return dfiber::doIo(sockfd, send_f, "send", static_cast<uint32_t>(dfiber::IOManager::Event::WRITE), SO_SNDTIMEO, buf, len, flags);
    }

    ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
    {
        return dfiber::doIo(sockfd, sendto_f, "sendto", static_cast<uint32_t>(dfiber::IOManager::Event::WRITE), SO_SNDTIMEO, buf, len, flags, dest_addr, addrlen);
    }

    ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
    {
        return dfiber::doIo(sockfd, sendmsg_f, "sendmsg", static_cast<uint32_t>(dfiber::IOManager::Event::WRITE), SO_SNDTIMEO, msg, flags);
    }

    int close(int fd)
    {
        if (!dfiber::tHookEnable) return close_f(fd);
        // 获得 FdManager 管理的 FdCtx 对象。
        std::shared_ptr<dfiber::FdCtx> ctx = dfiber::FdMgr::GetInstance()->get(fd);

        if (ctx)
        {
            // 删除 fd 以前，取消文件描述符 fd 上的所有事件，并触发所有回调函数。
            auto iom = dfiber::IOManager::GetThis();
            if (iom) iom->cancelAll(fd);

            dfiber::FdMgr::GetInstance()->del(fd);
        }


        // 处理完后调用原始系统调用，完成系统原生 fd 的删除。del() 函数不包含这部分的操作。
        return close_f(fd);
    }

    int fcntl(int fd, int cmd, ... /* arg */)
    {
        // va_list是在 C 语言中解决变参问题的定义的一个类型，常在 va_start(), va_arg(), and va_end() 宏中使用。变参问题是指参数的个数不定，可以是传入一个参数也可以是多个。可变参数中的每个参数的类型可以不同，也可以相同；可变参数的每个参数并没有实际的名称与之相对应，用起来是很灵活。
        // va_arg：访问下一个可变参数函数参数（函数宏）。
        // va_end：结束可变参数函数参数的遍历（函数宏）。
        // va_list：保存 va_start，va_arg，va_end 和 va_copy（typedef）所需的信息。
        // va_start：允许访问可变参数函数参数（函数宏）。
        // 使用流程:
        // 1. 首先在函数里定义一具 va_list 型的变量，这个变量是指向参数的指针；
        // 2. 然后用 va_start 宏初始化变量刚定义的 va_list 变量；
        // 3. 然后用 va_arg 返回可变的参数，va_arg 的第二个参数是你要返回的参数的类型（如果函数有多个可变参数的，依次调用 va_arg 获取各个参数）；
        // 4. 最后用 va_end 宏结束可变参数的获取。

        // va_list 类型持有处理可变参数的状态信息。
        va_list va;
        // 使其指向第一个可变参数（在 cmd 之后的参数）。
        va_start(va, cmd);

        switch (cmd)
        {
            // 用于设置文件描述符的状态标志（例如，设置非阻塞模式）。
            case F_SETFL:
            {
                // 获得下一个 int 类型的参数。
                int arg = va_arg(va, int);
                va_end(va);

                if (!dfiber::tHookEnable) return fcntl_f(fd, cmd, arg);

                std::shared_ptr<dfiber::FdCtx> ctx = dfiber::FdMgr::GetInstance()->get(fd);
                // 如果 ctx 无效，或者文件描述符关闭不是一个套接字就调用原始调用。
                if (!ctx || ctx->isClosed() || !ctx->isSocket()) return fcntl_f(fd, cmd, arg);

                // 和下面 ioctl() 思路一致，Hook 的前提是 socket 始终保持内核 nonblock。用户语义设置阻塞不代表框架可以把底层 nonblock 去掉，实际传给内核的标志需要保留框架自己的 nonblock 状态。

                // 用户是否设定了非阻塞。
                ctx->setUserNonblock(arg & O_NONBLOCK);
                // 最后是否阻塞根据系统设置决定。
                if (ctx->getSysNonblock())
                {
                    arg |= O_NONBLOCK;
                }
                else
                {
                    arg &= ~O_NONBLOCK;
                }
                return fcntl_f(fd, cmd, arg);
            }
            break;

            case F_GETFL:
            {
                va_end(va);

                // 调用原始的 fcntl 函数获取文件描述符的当前状态标志。
                int arg = fcntl_f(fd, cmd);

                if (!dfiber::tHookEnable) return arg;

                std::shared_ptr<dfiber::FdCtx> ctx = dfiber::FdMgr::GetInstance()->get(fd);
                // 如果上下文无效、文件描述符已关闭或不是套接字，则直接返回状态标志。
                if (!ctx || ctx->isClosed() || !ctx->isSocket()) return arg;

                // 同理，这里是呈现给用户显示的为用户设定的值，但是底层设置还是根据系统设置决定的。
                if (ctx->getUserNonblock())
                {
                    return arg | O_NONBLOCK;
                }
                else
                {
                    return arg & ~O_NONBLOCK;
                }
            }
            break;

            case F_DUPFD:
            case F_DUPFD_CLOEXEC:
            case F_SETFD:
            case F_SETOWN:
            case F_SETSIG:
            case F_SETLEASE:
            case F_NOTIFY:
#ifdef F_SETPIPE_SZ
            case F_SETPIPE_SZ:
#endif
            {
                int arg = va_arg(va, int);    // 从 va 获取标志位。
                va_end(va);                   // 清理 va。
                return fcntl_f(fd, cmd, arg); // 调用原始调用。
            }
            break;


            case F_GETFD:
            case F_GETOWN:
            case F_GETSIG:
            case F_GETLEASE:
#ifdef F_GETPIPE_SZ
            case F_GETPIPE_SZ:
#endif
            {
                va_end(va);              // 清理 va 变量。
                return fcntl_f(fd, cmd); // 返回原始调用的结果。
            }
            break;

            // 设置文件锁，如果不能立即获得锁，则返回失败。
            case F_SETLK:
            // 设置文件锁，且如果不能立即获得锁，则阻塞等待。
            case F_SETLKW:
            // 获取文件锁的状态。如果文件描述符 fd 关联的文件已经被锁定，那么该命令会填充 flock 结构体，指示锁的状态。
            case F_GETLK:
            {
                // 从可变参数列表中获取 struct flock* 类型的指针，这个指针指向一个 flock 结构体，包含锁定操作相关的信息（如锁的类型、偏移量、锁的长度等）。
                struct flock *arg = va_arg(va, struct flock *);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;

            // 获取文件描述符 fd 所属的所有者信息。这通常用于与信号处理相关的操作，尤其是在异步 I/O 操作中。
            case F_GETOWN_EX:
            // 设置文件描述符 fd 的所有者信息。
            case F_SETOWN_EX:
            {
                // 和上面的思路类似，从可变参数中提取相应类型的结构体指针。
                struct f_owner_exlock *arg = va_arg(va, struct f_owner_exlock *);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;

            default:
                va_end(va);
                return fcntl_f(fd, cmd);
        }
    }

    int ioctl(int fd, unsigned long request, ...)
    {
        // va_list 类型持有处理可变参数的状态信息。
        va_list va;
        // 初始化 va 让它指向可变参数的第一个参数位置。
        va_start(va, request);
        // 将 va 的指向参数的以 void* 类型取出存放到 arg 中。
        void *arg = va_arg(va, void *);
        // 用于结束对 va_list 变量的操作。清理 va 占用的资源。
        va_end(va);

        // 特殊处理：设置非阻塞模式的命令。
        if (FIONBIO == request)
        {
            if (!dfiber::tHookEnable) return ioctl_f(fd, request, arg);

            int flag = *(int *)arg;
            bool userNonblock = (0 != flag);

            std::shared_ptr<dfiber::FdCtx> ctx = dfiber::FdMgr::GetInstance()->get(fd);
            // 检查获取的上下文对象是否有效（即 ctx 是否为空）。如果上下文对象无效、文件描述符已关闭或不是一个套接字，则直接调用原始的 ioctl 函数，返回处理结果。
            if (!ctx || ctx->isClosed() || !ctx->isSocket()) return ioctl_f(fd, request, arg);

            // 如果上下文对象有效，调用 setUserNonblock 方法，将非阻塞模式设置为 userNonblock 指定的值。这将更新文件描述符的非阻塞状态。
            ctx->setUserNonblock(userNonblock);

            // Hook 的前提是 socket 始终保持内核 nonblock。用户语义改成阻塞不代表框架可以把底层 nonblock 去掉，实际传给内核的标志需要保留框架自己的 nonblock 状态。
            int sysFlag = ctx->getSysNonblock() || userNonblock;
            return ioctl_f(fd, request, &sysFlag);
        }


        return ioctl_f(fd, request, arg);
    }

    // 一个用于获取套接字选项值的函数。它允许你检查指定套接字的某些选项的当前设置。
    int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
    {
        return getsockopt_f(sockfd, level, optname, optval, optlen);
    }

    // 用于设置套接字的选项。它允许你对套接字的行为进行配置，如设置超时时间、缓冲区大小、地址重用等。
    int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
    {
        if (!dfiber::tHookEnable) return setsockopt_f(sockfd, level, optname, optval, optlen);

        // 如果 level 是 SOL_SOCKET 且 optname 是 SO_RCVTIMEO（接收超时）或 SO_SNDTIMEO（发送超时），获取与该文件描述符关联的 FdCtx 上下文对象，并设置超时时间。
        if (SOL_SOCKET == level)
        {
            if (SO_RCVTIMEO == optname || SO_SNDTIMEO == optname)
            {
                std::shared_ptr<dfiber::FdCtx> ctx = dfiber::FdMgr::GetInstance()->get(sockfd);
                if (ctx)
                {
                    // 那么代码会读取传入的 timeval 结构体，将其转化为毫秒数，并调用 ctx->setTimeout 方法，记录超时设置。
                    // struct timeval
                    // {
                    //     long tv_sec;  // 秒。
                    //     long tv_usec; // 微秒。
                    // };
                    const timeval *v = (const timeval *)optval;
                    ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
                }
            }
        }


        // 无论是否执行了超时处理，最后都会调用原始的 setsockopt_f 函数来设置实际的套接字选项。
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }
}
