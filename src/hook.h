#ifndef _DFIBER_HOOK_H_
#define _DFIBER_HOOK_H_

#include "globalmacros.h"

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <fcntl.h>


DFIBER_NAMESPACE_BEGIN(dfiber)


// 用于判断钩子功能是否启用。
bool isHookEnable();

// 用于设置钩子功能的启用或禁用状态。
void setHookEnable(bool flag);


// 使用 RAII 方式临时切换当前线程的 hook 开关。主要用于协程库内部需要绕过 hook、直接调用原始 syscall 的场景：
// 1. FdCtx 初始化 socket 时，需要直接操作底层 fcntl，避免在 FdManager 尚未完成发布时又重入到 hook/fd 管理逻辑。
// 2. IOManager/Scheduler 内部用于唤醒、idle、析构的 pipe/read/write/sleep 等基础操作，不应该再走协程化 hook，否则可能把框架自身再次卷入调度流程。
// 由于 hook 开关是 thread_local 状态，直接 setHookEnable(false) 很容易在提前 return、异常或较深调用链中漏掉恢复，导致当前线程后续逻辑继续以“hook 关闭”状态运行。因此这里保存进入作用域前的旧状态，并在离开作用域时自动恢复，保证“内部临时关闭 hook”不会泄漏到外部业务代码。类似于 std::lock_guard 的使用方法。
class HookEnableGuard
{

public:

    explicit HookEnableGuard(bool flag) : m_oldFlag(isHookEnable()) { setHookEnable(flag); }

    virtual ~HookEnableGuard() { setHookEnable(m_oldFlag); }


private:

    bool m_oldFlag;
};


DFIBER_NAMESPACE_END


// extern "C" 确保正确调用 C 库中的系统调用，C++ 编译器不会对这些函数名进行修饰。
// 下面的代码是一个用于网络钩子（hook）的功能，旨在拦截并重定向某些系统调用，如与网络相关的 socket、connect、read、write 等函数。这种技术通常用于网络库、异步 I/O 操作或协程库中，以便对底层系统调用进行自定义处理，从而实现非阻塞 I/O、超时控制或其他功能。
extern "C" // 确保正确调用c库中的系统调用，c++编译器不会对这些函数名进行修饰。
{
    // track the original version
    // C 函数原型声明与重定向（第 18-120 行）。
    typedef unsigned int (*sleep_fun)(unsigned int seconds);
    extern sleep_fun sleep_f;

    typedef int (*usleep_fun)(useconds_t usec);

    extern usleep_fun usleep_f;

    typedef int (*nanosleep_fun)(const struct timespec *req, struct timespec *rem);
    extern nanosleep_fun nanosleep_f;

    typedef int (*socket_fun)(int domain, int type, int protocol);
    extern socket_fun socket_f;

    typedef int (*connect_fun)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
    extern connect_fun connect_f;

    typedef int (*accept_fun)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
    extern accept_fun accept_f;

    typedef ssize_t (*read_fun)(int fd, void *buf, size_t count);
    extern read_fun read_f;

    typedef ssize_t (*readv_fun)(int fd, const struct iovec *iov, int iovcnt);
    extern readv_fun readv_f;

    typedef ssize_t (*recv_fun)(int sockfd, void *buf, size_t len, int flags);
    extern recv_fun recv_f;

    typedef ssize_t (*recvfrom_fun)(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
    extern recvfrom_fun recvfrom_f;

    typedef ssize_t (*recvmsg_fun)(int sockfd, struct msghdr *msg, int flags);
    extern recvmsg_fun recvmsg_f;

    typedef ssize_t (*write_fun)(int fd, const void *buf, size_t count);
    extern write_fun write_f;
    typedef ssize_t (*writev_fun)(int fd, const struct iovec *iov, int iovcnt);
    extern writev_fun writev_f;

    typedef ssize_t (*send_fun)(int sockfd, const void *buf, size_t len, int flags);
    extern send_fun send_f;

    typedef ssize_t (*sendto_fun)(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen);
    extern sendto_fun sendto_f;

    typedef ssize_t (*sendmsg_fun)(int sockfd, const struct msghdr *msg, int flags);
    extern sendmsg_fun sendmsg_f;

    typedef int (*close_fun)(int fd);
    extern close_fun close_f;

    typedef int (*fcntl_fun)(int fd, int cmd, ... /* arg */);
    extern fcntl_fun fcntl_f;

    typedef int (*ioctl_fun)(int fd, unsigned long request, ...);
    extern ioctl_fun ioctl_f;

    typedef int (*getsockopt_fun)(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
    extern getsockopt_fun getsockopt_f;

    typedef int (*setsockopt_fun)(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
    extern setsockopt_fun setsockopt_f;

    // function prototype -> 对应 .h 中已经存在，可以省略。
    // 函数重定义（第 85-120 行）。可以通过判断是否启用了钩子来决定是调用原始的系统函数，还是执行自定义的逻辑。
    // sleep function
    unsigned int sleep(unsigned int seconds);
    int usleep(useconds_t usce);
    int nanosleep(const struct timespec *req, struct timespec *rem);

    // socket funciton
    int socket(int domain, int type, int protocol);
    int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
    int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

    // read
    ssize_t read(int fd, void *buf, size_t count);
    ssize_t readv(int fd, const struct iovec *iov, int iovcnt);

    ssize_t recv(int sockfd, void *buf, size_t len, int flags);
    ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
    ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags);


    // write
    ssize_t write(int fd, const void *buf, size_t count);
    ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

    ssize_t send(int sockfd, const void *buf, size_t len, int flags);
    ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen);
    ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags);

    // fd
    int close(int fd);

    // socket control
    int fcntl(int fd, int cmd, ... /* arg */);
    int ioctl(int fd, unsigned long request, ...);

    int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
    int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
}


#endif
