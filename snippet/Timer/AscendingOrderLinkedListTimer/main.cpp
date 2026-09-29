#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <atomic>

#include "timer.h"


TEST(TimerManagerTest, EmptyTick)
{
    TimerManager tmr;

    EXPECT_NO_THROW(
        {
            tmr.tick();
        });
}

TEST(TimerManagerTest, OneTimer)
{
    TimerManager tmr;
    std::atomic_int count = 0;
    uint64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();

    tmr.addTimer(
        new Timer(
            now + 100,
            [&]()
            {
                ++count;
            }));

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    EXPECT_EQ(count, 0);
    tmr.tick();
    EXPECT_EQ(count, 1);
}

TEST(TimerManagerTest, MultipleTimer)
{
    TimerManager tmr;
    std::vector<int> res;
    uint64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();

    tmr.addTimer(new Timer(now + 300,
                           [&]
                           {
                               res.push_back(3);
                           }));

    tmr.addTimer(new Timer(now + 100,
                           [&]
                           {
                               res.push_back(1);
                           }));

    tmr.addTimer(new Timer(now + 200,
                           [&]
                           {
                               res.push_back(2);
                           }));

    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    tmr.tick();

    ASSERT_EQ(res.size(), 3);

    EXPECT_EQ(res[0], 1);
    EXPECT_EQ(res[1], 2);
    EXPECT_EQ(res[2], 3);
}

TEST(TimerManagerTest, NotExpire)
{
    TimerManager tmr;
    bool called = false;
    uint64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();

    tmr.addTimer(new Timer(now + 500,
                           [&]
                           {
                               called = true;
                           }));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    tmr.tick();
    EXPECT_FALSE(called);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    tmr.tick();
    EXPECT_TRUE(called);
}

TEST(TimerManagerTest, AdjustTimer)
{
    TimerManager tmr;
    bool called = false;
    uint64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();

    auto timer = new Timer(now + 100,
                           [&]
                           {
                               called = true;
                           });

    tmr.addTimer(timer);

    // 延长到 500ms。
    tmr.adjustTimer(timer, now + 500);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    tmr.tick();
    EXPECT_FALSE(called);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    tmr.tick();
    EXPECT_TRUE(called);
}

TEST(TimerManagerTest, DeleteTimer)
{
    TimerManager tmr;
    bool called = false;
    uint64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();

    auto timer = new Timer(now + 100,
                           [&]
                           {
                               called = true;
                           });

    tmr.addTimer(timer);

    tmr.delTimer(timer);
    delete timer;

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    tmr.tick();
    EXPECT_FALSE(called);
}

TEST(TimerManagerTest, TickManyTimes)
{
    TimerManager tmr;
    int count = 0;
    uint64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();

    tmr.addTimer(new Timer(now + 100, [&]
                           { ++count; }));
    tmr.addTimer(new Timer(now + 200, [&]
                           { ++count; }));
    tmr.addTimer(new Timer(now + 300, [&]
                           { ++count; }));

    std::this_thread::sleep_for(std::chrono::milliseconds(110));
    tmr.tick();
    EXPECT_EQ(count, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(110));
    tmr.tick();
    EXPECT_EQ(count, 2);

    std::this_thread::sleep_for(std::chrono::milliseconds(110));
    tmr.tick();
    EXPECT_EQ(count, 3);
}

#if DFIBER_CONFIG_DEBUG

TEST(TimerManagerTest, InsertOrder)
{
    TimerManager tmr;

    tmr.addTimer(new Timer(300, [] {}));
    tmr.addTimer(new Timer(100, [] {}));
    tmr.addTimer(new Timer(200, [] {}));

    tmr.printTimers();
}

#endif


int main(int argc, char *argv[])
{
    ::testing::InitGoogleTest(&argc, argv);


    return RUN_ALL_TESTS();
}
