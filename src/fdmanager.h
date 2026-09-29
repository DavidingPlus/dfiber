#ifndef _DFIBER_FDMANAGER_H_
#define _DFIBER_FDMANAGER_H_

#include <memory>
#include <vector>
#include <shared_mutex>
#include <mutex>

#include <sys/socket.h>


// FdCtx 类主要用于管理与文件描述符相关的状态和操作。在用户态记录了 fd 的读写超时和非阻塞信息，其中非阻塞包括用户显示设置的非阻塞和 hook 内部设置的非阻塞，区分这两种非阻塞可以有效应对用户对 fd 设置/获取 NONBLOCK 模式的情形。
class FdCtx : public std::enable_shared_from_this<FdCtx>
{

public:

    // 构造 FdCtx 对象。参数 fd 必须是一个已经存在且有效的 Linux 文件描述符。FdCtx 不负责创建文件描述符，而是对已有 fd 进行状态管理，包括检测 fd 类型、记录非阻塞状态以及维护超时信息。如果传入无效 fd，则初始化失败，isInit() 将返回 false。
    FdCtx(int fd) : m_fd(fd) { init(); }

    ~FdCtx() = default;

    // 初始化 FdCtx 对象。
    bool init();

    bool isInit() const { return m_isInit; }

    bool isSocket() const { return m_isSocket; }

    bool isClosed() const { return m_isClosed; }

    // 设置和获取用户层面的非阻塞状态。
    void setUserNonblock(bool v) { m_userNonblock = v; }

    bool getUserNonblock() const { return m_userNonblock; }

    // 设置和获取系统层面的非阻塞状态。
    void setSysNonblock(bool v) { m_sysNonblock = v; }

    bool getSysNonblock() const { return m_sysNonblock; }

    // 设置超时时间，type 用于区分读事件和写事件的超时设置，v 表示时间毫秒。
    void setTimeout(int type, uint64_t v);

    // 获取超时时间。
    uint64_t getTimeout(int type) { return SO_SNDTIMEO == type ? m_sendTimeout : m_recvTimeout; }


private:

    // 标记文件描述符是否已初始化。
    bool m_isInit = false;

    // 标记文件描述符是否是一个套接字。
    bool m_isSocket = false;

    // 标记是否由框架（Hook）将 fd 设置为非阻塞模式。为了实现协程调度，所有 Socket 都需要工作在非阻塞模式下，该状态仅供框架内部使用，不代表用户主动设置了 O_NONBLOCK。
    bool m_sysNonblock = false;

    // 标记是否由用户主动将 fd 设置为非阻塞模式。该状态用于保存用户的真实意图，Hook 在处理 fcntl/ioctl 等系统调用时需要依据该状态，保证对用户表现出的行为与 Linux 原生接口保持一致。
    bool m_userNonblock = false;

    // 标记文件描述符是否已关闭。
    bool m_isClosed = false;

    // 文件描述符的整数值。
    int m_fd = -1;

    // 读事件的超时时间，默认为 -1 表示没有超时限制。
    uint64_t m_recvTimeout = static_cast<uint64_t>(-1);

    // 写事件的超时时间，默认为 -1 表示没有超时限制。
    uint64_t m_sendTimeout = static_cast<uint64_t>(-1);
};


// 用于管理 FdCtx 对象的集合。它提供了对文件描述符上下文的访问和管理功能。
class FdManager
{

public:

    // 构造函数。
    FdManager() { m_datas.resize(64); }

    virtual ~FdManager() = default;

    // 获取指定文件描述符的 FdCtx 对象。如果 autoCreate 为 true，在不存在时自动创建新的 FdCtx 对象。
    std::shared_ptr<FdCtx> get(int fd, bool autoCreate = false);

    // 删除指定文件描述符的 FdCtx 对象。
    void del(int fd);


private:

    // 用于保护对 m_datas 的访问，支持共享读锁和独占写锁。
    std::shared_mutex m_mutex;

    // 存储所有 FdCtx 对象的共享指针。
    std::vector<std::shared_ptr<FdCtx>> m_datas;
};


// 单例模板类。用于保证指定类型 T 在整个程序生命周期内只有一个全局实例，并提供统一的全局访问入口。
// 该实现采用懒加载方式：只有第一次调用 GetInstance() 时才会创建对象。同时通过互斥锁保证多线程环境下实例创建过程的线程安全。
template <typename T>
class Singleton
{

public:

    static T *GetInstance()
    {
        // 加锁。
        std::lock_guard<std::mutex> lock(mutex);
        if (!instance)
        {
            instance = new T();
        }


        // 提高对外的访问点，在系统生命周期中一般一个类只有一个全局实例。
        return instance;
    }

    static void DestroyInstance()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (instance)
        {
            delete instance;
            instance = nullptr; // 防止野指针。
        }
    }

    Singleton(const Singleton &) = delete;

    Singleton &operator=(const Singleton &) = delete;


protected:

    Singleton() = default;

    virtual ~Singleton() = default;


private:

    // 对外提供的实例。
    static T *instance;

    // 互斥锁。
    static std::mutex mutex;
};


// 重定义将 Singleton<FdManager> 变成 FdMgr 的缩写。
// 显然 FdManager 类需要作为单例模式使用，确保类只有一个全局实例，并提供全局访问点。
using FdMgr = Singleton<FdManager>;


#endif
