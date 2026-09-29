#include "fdmanager.h"

#include "hook.h"

#include <sys/stat.h>
#include <fcntl.h>


DFIBER_NAMESPACE_BEGIN(dfiber)


// 显式实例化 Singleton<FdManager> 模板。使编译器生成 FdManager 对应的单例模板代码，避免模板成员在多个编译单元中重复实例化。
template class Singleton<FdManager>;

// 模板静态成员变量定义。类内只声明了静态成员，这里需要在类外进行定义和初始化。每一种模板类型都会拥有独立的一份静态成员变量。
template <typename T>
T *Singleton<T>::instance = nullptr;

template <typename T>
std::mutex Singleton<T>::mutex;


bool FdCtx::init()
{
    // 如果已经初始化过了就直接返回 true。
    if (m_isInit) return true;

    // fstat 函数用于获取与文件描述符 m_fd 关联的文件状态信息存放到 statbuf 中。如果 fstat() 返回 -1，表示文件描述符无效或出现错误。
    struct stat statbuf;
    if (-1 == fstat(m_fd, &statbuf))
    {
        m_isInit = false;
        m_isSocket = false;
    }
    else
    {
        m_isInit = true;
        // S_ISSOCK(statbuf.st_mode) 用于检查 st_mode 中的位，以确定文件是否是一个套接字（socket）。该宏定义在 <sys/stat.h> 头文件中。
        m_isSocket = S_ISSOCK(statbuf.st_mode);
    }

    // 如果是一个套接字，设置为非阻塞。
    if (m_isSocket)
    {
        {
            HookEnableGuard guard(false);

            // 获取文件描述符的状态，检查当前标志中是否已经设置了非阻塞标志，如果没有则设置。
            int flags = fcntl(m_fd, F_GETFL, 0);
            if (!(flags & O_NONBLOCK)) fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);
        }

        // 标记由框架（Hook）将 fd 设置为了非阻塞模式。
        m_sysNonblock = true;
    }
    else
    {
        m_sysNonblock = false;
    }


    return m_isInit; // 即初始化是否成功。
}

void FdCtx::setTimeout(int type, uint64_t v)
{
    // type 指定超时类型的标志。可能的值包括 SO_RCVTIMEO 和 SO_SNDTIMEO，分别用于接收超时和发送超时。v 代表设置的超时时间，单位是毫秒或者其他。
    // 如果 type 类型的读事件，则超时事件设置到 recvtimeout 上，否则就设置到 sendtimeout 上。
    if (SO_RCVTIMEO == type)
    {
        m_recvTimeout = v;
    }
    else
    {
        m_sendTimeout = v;
    }
}

std::shared_ptr<FdCtx> FdManager::get(int fd, bool autoCreate)
{
    // 文件描述符无效则直接返回。
    if (-1 == fd) return nullptr;

    std::shared_lock<std::shared_mutex> readLock(m_mutex);
    // 如果 fd 超出了 m_datas 的范围，并且 autoCreate 为 false，则返回 nullptr，表示没有创建新对象的需求。
    if (m_datas.size() <= fd)
    {
        if (!autoCreate) return nullptr;
    }
    // 如果 fd 在 m_datas 范围内，如果 m_datas[fd] 有数据即不为空，直接返回。如果为空但不允许创建，直接返回 nullptr，二者条件综合就是下面的这个式子。为空但允许创建就会走到下面扩容再创建的分支。
    else
    {
        if (m_datas[fd] || !autoCreate) return m_datas[fd];
    }

    // 当 fd 的大小超出 m_data.size() 的值也就是 m_datas[fd] 数组中没找到对应的 fd 并且 autoCreate 为 true 时候会走到这里，先扩容，然后创建新对象。
    readLock.unlock();
    std::unique_lock<std::shared_mutex> writeLock(m_mutex);

    // 扩容 m_datas 数组，使其能够存储 fd 对应的 FdCtx。使用 fd * 3 / 2 + 1 而不是 fd * 1.5 进行扩容，相比直接扩容到 fd + 1 可以减少频繁扩容次数，同时避免使用浮点数计算带来的隐式类型转换问题。例如 1 * 1.5 -> 1(int)
    if (m_datas.size() <= fd) m_datas.resize(fd * 3 / 2 + 1);

    m_datas[fd] = std::make_shared<FdCtx>(fd);


    return m_datas[fd];
}

void FdManager::del(int fd)
{
    std::unique_lock<std::shared_mutex> writeLock(m_mutex);

    if (fd >= m_datas.size()) return;

    // reset() 用于释放当前 shared_ptr 对对象的所有权，并将智能指针置为 nullptr。调用 reset() 后，该 shared_ptr 不再持有对象，对象的引用计数会减少。如果当前 shared_ptr 是管理该对象的最后一个所有者，则对象会被自动销毁。
    m_datas[fd].reset();
}


DFIBER_NAMESPACE_END
