#include <iostream>
#include <chrono>
#include <thread>

#include "timer.h"


void func(int i)
{
    std::cout << "i: " << i << std::endl;
}


int main(int argc, char const *argv[])
{
    std::shared_ptr<dfiber::TimerManager> manager(new dfiber::TimerManager());
    std::vector<std::function<void()>> cbs;

    // 测试 listExpiredCb 超时功能。
    {
        for (int i = 0; i < 10; ++i) manager->addTimer((i + 1) * 1000, std::bind(&func, i), false);
        std::cout << "all timers have been set up" << std::endl;

        std::this_thread::sleep_for(std::chrono::seconds(5));
        manager->listExpiredCb(cbs);
        while (!cbs.empty())
        {
            std::function<void()> cb = *cbs.begin();
            cbs.erase(cbs.begin());
            cb();
        }

        std::this_thread::sleep_for(std::chrono::seconds(5));
        manager->listExpiredCb(cbs);
        while (!cbs.empty())
        {
            std::function<void()> cb = *cbs.begin();
            cbs.erase(cbs.begin());
            cb();
        }
    }

    // 测试 recurring。
    {
        manager->addTimer(1000, std::bind(&func, 1000), true);
        for (int i = 0; i < 10; ++i)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            manager->listExpiredCb(cbs);
            std::function<void()> cb = *cbs.begin();
            cbs.erase(cbs.begin());
            cb();
        }
    }
}
