#include <gtest/gtest.h>

#include "fdmanager.h"

#include <thread>

#include <fcntl.h>


TEST(FdCtxTest, InvalidFd)
{
    FdCtx ctx(-1);

    EXPECT_FALSE(ctx.isInit());
    EXPECT_FALSE(ctx.isSocket());
}

TEST(FdCtxTest, RegularFile)
{
    int fd = open("/tmp/fdctx_test_file", O_CREAT | O_RDWR, 0644);
    ASSERT_GE(fd, 0);

    FdCtx ctx(fd);

    EXPECT_TRUE(ctx.isInit());
    EXPECT_FALSE(ctx.isSocket());

    EXPECT_FALSE(ctx.getSysNonblock());

    close(fd);
}

TEST(FdCtxTest, SocketNonblock)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    FdCtx ctx(fd);

    EXPECT_TRUE(ctx.isInit());
    EXPECT_TRUE(ctx.isSocket());

    // 框架应该设置非阻塞。
    EXPECT_TRUE(ctx.getSysNonblock());

    EXPECT_TRUE(fcntl(fd, F_GETFL) & O_NONBLOCK);

    close(fd);
}

TEST(FdCtxTest, UserNonblock)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    FdCtx ctx(fd);

    EXPECT_FALSE(ctx.getUserNonblock());

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    ctx.setUserNonblock(true);

    EXPECT_TRUE(ctx.getUserNonblock());

    close(fd);
}

TEST(FdCtxTest, Timeout)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    FdCtx ctx(fd);

    ctx.setTimeout(SO_RCVTIMEO, 1000);
    EXPECT_EQ(ctx.getTimeout(SO_RCVTIMEO), 1000);

    ctx.setTimeout(SO_SNDTIMEO, 2000);
    EXPECT_EQ(ctx.getTimeout(SO_SNDTIMEO), 2000);

    close(fd);
}

TEST(FdManagerTest, NoAutoCreate)
{
    FdManager mgr;

    auto ctx = mgr.get(100, false);

    EXPECT_EQ(ctx, nullptr);
}

TEST(FdManagerTest, AutoCreate)
{
    int fd = open("/tmp/fdmanager_test", O_CREAT | O_RDWR, 0644);
    ASSERT_GE(fd, 0);

    FdManager mgr;
    auto ctx = mgr.get(fd, true);

    ASSERT_NE(ctx, nullptr);
    EXPECT_TRUE(ctx->isInit());

    close(fd);
}

TEST(FdManagerTest, SameContext)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    FdManager mgr;

    auto ctx1 = mgr.get(fd, true);
    auto ctx2 = mgr.get(fd, false);

    ASSERT_NE(ctx1, nullptr);
    ASSERT_NE(ctx2, nullptr);


    EXPECT_EQ(ctx1.get(), ctx2.get());

    close(fd);
}

TEST(FdManagerTest, DeleteContext)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    FdManager mgr;

    auto ctx = mgr.get(fd, true);
    ASSERT_NE(ctx, nullptr);

    mgr.del(fd);

    auto ctx2 = mgr.get(fd, false);
    EXPECT_EQ(ctx2, nullptr);

    close(fd);
}

TEST(FdManagerTest, LargeFd)
{
    FdManager mgr;
    int fd = -1;

    for (int i = 0; i < 10000; ++i)
    {
        fd = open(std::string("/tmp/fdmanager_large_test" + std::to_string(i)).data(), O_CREAT | O_RDWR, 0644);
        ASSERT_GE(fd, 0);

        auto ctx = mgr.get(fd, true);

        ASSERT_NE(ctx, nullptr);
        EXPECT_TRUE(ctx->isInit());

        close(fd);
    }
}

TEST(FdManagerTest, ConcurrentGet)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    FdManager mgr;
    std::vector<std::thread> threads;
    std::vector<std::shared_ptr<FdCtx>> res(10);

    for (int i = 0; i < 10; ++i)
    {
        threads.emplace_back(
            [&mgr, &res, fd, i]()
            {
                res[i] = mgr.get(fd, true);
            });
    }

    for (auto &t : threads) t.join();

    for (int i = 1; i < 10; ++i) EXPECT_EQ(res[0].get(), res[i].get());

    close(fd);
}

TEST(SingletonTest, Instance)
{
    auto a = FdMgr::GetInstance();
    auto b = FdMgr::GetInstance();

    EXPECT_EQ(a, b);
}
