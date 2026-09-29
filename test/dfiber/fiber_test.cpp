#include <gtest/gtest.h>

#include "fiber.h"

#include "config.h"


using namespace dfiber;


// Fiber 析构输出出现在测试结束之后，出现 Fiber ID 为 1 和较大的 Fiber ID（例如 id = 1164）属于正常现象。
//
// 1. 关于主协程（Main Fiber）：
// 主协程由 Fiber::GetThis() 第一次调用时创建，并由线程局部变量 thread_local std::shared_ptr<Fiber> t_threadFiber 持有。由于 thread_local 对象的生命周期与线程绑定，而不是与单个测试用例绑定，因此即使某个单元测试已经执行结束，主协程仍然会被 t_threadFiber 持有，不会立即触发析构。当测试程序执行结束，GoogleTest 完成所有测试并进入线程退出阶段时，thread_local 对象开始析构，shared_ptr 引用计数归零，最终调用 Fiber 析构函数。
// 因此会在所有测试结果输出完成后看到：
//
// [----------] Global test environment tear-down
// [==========] 31 tests from 3 test suites ran. (570 ms total)
// [  PASSED  ] 31 tests.
// ~Fiber(): id = 1
//
// 该现象属于正常的资源释放流程，并不表示 Fiber 泄漏。
//
// 2. 关于较大的 Fiber ID（例如 id = 1164）：
// Fiber ID 采用全局递增方式生成，例如 static std::atomic<uint64_t> s_fiber_id 每创建一个 Fiber，ID 就会递增一次。该 ID 仅表示 Fiber 的创建顺序，不会因为 Fiber 析构或者测试用例结束而重新从 1 开始。在多个测试用例执行过程中，已经创建并销毁了大量 Fiber。后续压力测试（StressTest）又会继续创建大量 Fiber，因此后面创建的 Fiber 可能拥有较大的编号，例如 Fiber(): child id = 1164。当该 Fiber 生命周期结束，引用计数归零后，会执行 ~Fiber(): id = 1164。这里的 1164 表示该 Fiber 是整个程序运行期间创建的第 1164 个 Fiber，并不表示当前同时存在 1164 个 Fiber，也不表示前面的 Fiber 没有释放。因此：~Fiber(): id = 1164 与：~Fiber(): id = 1 分别对应普通工作 Fiber 和线程主协程的析构过程，二者析构时机不同，都属于正常现象。
TEST(FiberTest, GetThis)
{
    auto fiber = Fiber::GetThis();

    ASSERT_NE(fiber, nullptr);
    EXPECT_EQ(fiber->getState(), Fiber::RUNNING);
}

TEST(FiberTest, CreateFiber)
{
    Fiber::GetThis();

    auto fiber = std::make_shared<Fiber>(
        [] {},
        0,
        false);

    ASSERT_NE(fiber, nullptr);
    EXPECT_EQ(fiber->getState(), Fiber::READY);
}

TEST(FiberTest, Resume)
{
    Fiber::GetThis();

    bool executed = false;

    auto fiber = std::make_shared<Fiber>(
        [&]
        {
            executed = true;
        },
        0,
        false);

    EXPECT_FALSE(executed);

    fiber->resume();

    EXPECT_TRUE(executed);
    EXPECT_EQ(fiber->getState(), Fiber::TERMINATE);
}

TEST(FiberTest, Reset)
{
    Fiber::GetThis();

    int value = 0;

    auto fiber = std::make_shared<Fiber>(
        [&]
        {
            value = 1;
        },
        0,
        false);

    EXPECT_EQ(value, 0);

    fiber->resume();
    EXPECT_EQ(value, 1);

    fiber->reset([&]
                 {
                     value = 2; //
                 });
    EXPECT_EQ(fiber->getState(), Fiber::READY);

    fiber->resume();
    EXPECT_EQ(value, 2);
}

TEST(FiberTest, GetFiberId)
{
    auto fiber = Fiber::GetThis();

    EXPECT_EQ(Fiber::GetFiberId(), fiber->getId());
}

TEST(FiberTest, FiberIdUnique)
{
    Fiber::GetThis();

    auto f1 = std::make_shared<Fiber>([] {}, 0, false);
    auto f2 = std::make_shared<Fiber>([] {}, 0, false);

    EXPECT_NE(f1->getId(), f2->getId());
}

TEST(FiberTest, GetThisInsideFiber)
{
    Fiber::GetThis();

    Fiber *current = nullptr;

    auto fiber = std::make_shared<Fiber>(
        [&]
        {
            current = Fiber::GetThis().get();
        },
        0,
        false);

    fiber->resume();

    EXPECT_EQ(current, fiber.get());
}

TEST(FiberTest, MultipleResume)
{
    Fiber::GetThis();

    int count = 0; // 同一个线程中的协程是依次执行的，因此不存在竞争需要加锁的问题，也就不需要使用原子变量。

    std::vector<std::shared_ptr<Fiber>> fibers;

    for (int i = 0; i < 5; ++i)
    {
        fibers.push_back(std::make_shared<Fiber>(
            [&]
            {
                ++count;
            },
            0,
            false));
    }

    for (auto &fiber : fibers) fiber->resume();

    EXPECT_EQ(count, 5);
}

TEST(FiberTest, ResumeAfterTerminate)
{
    Fiber::GetThis();

    auto fiber = std::make_shared<Fiber>(
        [] {},
        0,
        false);

    // 已经结束的协程不能再次 resume()，需要 reset() 才能再次运行。
    fiber->resume();

    // Release 模式下，assert(xxx) 宏会被定义为 NDEBUG，该部分的代码都会被优化掉，因此下面这行代码只在 Debug 下测试。
    if (DFIBER_CONFIG_DEBUG) ASSERT_DEATH({ fiber->resume(); }, "");
}
