#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "timer.h"


TEST(TimerTest, OneShotTimer)
{
    TimerManager manager;
    std::atomic_bool called = false;

    manager.addTimer(
        100,
        [&]()
        {
            called = true;
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    std::vector<std::function<void()>> cbs;
    manager.listExpiredCb(cbs);

    EXPECT_EQ(cbs.size(), 1);

    for (auto &cb : cbs) cb();

    EXPECT_TRUE(called);
}

TEST(TimerTest, TimerNotExpired)
{
    TimerManager manager;
    std::atomic_bool called = false;

    manager.addTimer(
        500,
        [&]()
        {
            called = true;
        });

    std::vector<std::function<void()>> cbs;
    manager.listExpiredCb(cbs);

    EXPECT_TRUE(cbs.empty());
    EXPECT_FALSE(called);
}

TEST(TimerTest, CancelTimer)
{
    TimerManager manager;
    std::atomic_bool called = false;

    auto timer = manager.addTimer(
        100,
        [&]()
        {
            called = true;
        });

    EXPECT_TRUE(timer->cancel());

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    std::vector<std::function<void()>> cbs;
    manager.listExpiredCb(cbs);

    EXPECT_TRUE(cbs.empty());
    EXPECT_FALSE(called);
}

TEST(TimerTest, RefreshTimer)
{
    TimerManager manager;
    std::atomic_bool called = false;

    auto timer = manager.addTimer(
        100,
        [&]()
        {
            called = true;
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_TRUE(timer->refresh());

    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    std::vector<std::function<void()>> cbs;
    manager.listExpiredCb(cbs);

    EXPECT_TRUE(cbs.empty());

    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    manager.listExpiredCb(cbs);

    EXPECT_EQ(cbs.size(), 1);

    for (auto &cb : cbs) cb();

    EXPECT_TRUE(called);
}

TEST(TimerTest, ResetTimer)
{
    TimerManager manager;
    std::atomic_bool called = false;

    auto timer = manager.addTimer(
        100,
        [&]()
        {
            called = true;
        });

    EXPECT_TRUE(timer->reset(300, true));

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    std::vector<std::function<void()>> cbs;
    manager.listExpiredCb(cbs);

    EXPECT_TRUE(cbs.empty());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    manager.listExpiredCb(cbs);

    EXPECT_EQ(cbs.size(), 1);

    for (auto &cb : cbs) cb();

    EXPECT_TRUE(called);
}

TEST(TimerTest, RecurringTimer)
{
    TimerManager manager;
    std::atomic_int count = 0;

    manager.addTimer(
        100,
        [&]()
        {
            ++count;
        },
        true);

    for (int i = 0; i < 3; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));

        std::vector<std::function<void()>> cbs;
        manager.listExpiredCb(cbs);

        for (auto &cb : cbs) cb();
    }

    EXPECT_EQ(count.load(), 3);
}

TEST(TimerTest, HasTimer)
{
    TimerManager manager;

    EXPECT_FALSE(manager.hasTimer());
    manager.addTimer(100, [] {});
    EXPECT_TRUE(manager.hasTimer());
}

TEST(TimerTest, GetNextTimer)
{
    TimerManager manager;

    manager.addTimer(200, [] {});

    auto next = manager.getNextTimer();

    // 介于 0 到 200 ms 之间。
    EXPECT_LE(next, 200u);
    EXPECT_GT(next, 0u);
}

TEST(TimerTest, CancelTwice)
{
    TimerManager manager;

    auto timer = manager.addTimer(100, [] {});

    EXPECT_TRUE(timer->cancel());
    EXPECT_FALSE(timer->cancel());
}

TEST(TimerTest, RefreshAfterCancel)
{
    TimerManager manager;

    auto timer = manager.addTimer(100, [] {});

    EXPECT_TRUE(timer->cancel());
    EXPECT_FALSE(timer->refresh());
}

TEST(TimerTest, ResetAfterCancel)
{
    TimerManager manager;

    auto timer = manager.addTimer(100, [] {});

    EXPECT_TRUE(timer->cancel());
    EXPECT_FALSE(timer->reset(200, true));
}

TEST(TimerTest, GetNextTimerEmpty)
{
    TimerManager manager;

    std::cout << manager.getNextTimer() << std::endl;
    EXPECT_EQ(manager.getNextTimer(), ~0ull);
}

TEST(TimerTest, MultiTimerOrder)
{
    TimerManager manager;
    std::vector<int> vec;

    manager.addTimer(300, [&]
                     { vec.push_back(3); });
    manager.addTimer(100, [&]
                     { vec.push_back(1); });
    manager.addTimer(200, [&]
                     { vec.push_back(2); });

    for (int i = 0; i < 3; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));

        std::vector<std::function<void()>> cbs;
        manager.listExpiredCb(cbs);

        for (auto &cb : cbs) cb();
    }

    ASSERT_EQ(vec.size(), 3u);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(vec[i], 1 + i);
}

TEST(TimerTest, ConditionTimerAlive)
{
    TimerManager manager;
    auto obj = std::make_shared<int>(1);
    bool called = false;

    manager.addConditionTimer(
        100,
        [&]
        {
            called = true;
        },
        obj);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    std::vector<std::function<void()>> cbs;
    manager.listExpiredCb(cbs);

    for (auto &cb : cbs) cb();
    EXPECT_TRUE(called);
}

TEST(TimerTest, ConditionTimerExpiredObject)
{
    TimerManager manager;
    auto obj = std::make_shared<int>(1);
    bool called = false;

    manager.addConditionTimer(
        100,
        [&]
        {
            called = true;
        },
        obj);

    // 提前销毁对象。
    obj.reset();

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    std::vector<std::function<void()>> cbs;
    manager.listExpiredCb(cbs);

    for (auto &cb : cbs) cb();
    EXPECT_FALSE(called);
}
