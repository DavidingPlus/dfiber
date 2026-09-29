#include "timer.h"

#include "config.h"

#include <iostream>
#include <chrono>


size_t TimerManager::getParentIndex(size_t index) { return (index - 1) / 2; }

size_t TimerManager::getLeftChildIndex(size_t index) { return index * 2 + 1; }

size_t TimerManager::getRightChildIndex(size_t index) { return index * 2 + 2; }


TimerManager::TimerId TimerManager::add(uint64_t expire, Timer::Callback cb)
{
    TimerId id = m_nextId++;

    m_heap.emplace_back(expire, std::move(cb), id);
    m_timerIndex[id] = m_heap.size() - 1;

    // 新插入节点只可能破坏它与父节点之间的堆序。每次交换后，原父节点下沉，新父结点的值只会更小，因此会小于自己的两个孩子，因此下面的子树依旧满足堆性质；唯一可能出现问题的是新的父子关系，所以继续向上调整即可。
    heapifyUp(m_heap.size() - 1);


    return id;
}

void TimerManager::cancel(TimerId id)
{
    auto it = m_timerIndex.find(id);
    if (m_timerIndex.end() == it) return;

    remove(it->second);
}

void TimerManager::tick()
{
    // 获取当前时间，用于判断定时器是否到期。
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

    if (DFIBER_CONFIG_DEBUG) std::cout << "TimerManager::tick() now: " << now << std::endl;

    while (!m_heap.empty())
    {
        // 最小堆的堆顶永远保存最早到期的定时器。如果堆顶定时器还未到期，说明其它定时器也不可能到期，直接退出。
        if (now < m_heap[0].m_expire) break;

        // 保存堆顶定时器。先删除定时器再执行回调，避免回调函数内部修改 TimerManager 导致堆结构变化。
        Timer timer = m_heap[0];

        // 删除已经到期的定时器，并通过 heapifyDown() 恢复最小堆性质。
        pop();

        // 执行定时器回调。
        if (timer.m_callback) timer.m_callback();
    }
}

void TimerManager::heapifyUp(size_t index)
{
    while (index)
    {
        size_t parent = TimerManager::getParentIndex(index);

        // 注意边界条件，相同的时候也直接返回。
        if (m_heap[parent].m_expire <= m_heap[index].m_expire) break;

        swapNode(parent, index);

        index = parent;
    }
}

void TimerManager::heapifyDown(size_t index)
{
    while (index < m_heap.size())
    {
        size_t left = getLeftChildIndex(index);
        size_t right = getRightChildIndex(index);

        // 完全二叉树中，没有左孩子，一定没有右孩子，说明当前节点是叶子节点。
        if (left >= m_heap.size()) break;

        // 默认左孩子较小，选择左孩子。
        size_t smaller = left;

        // 如果右孩子存在，并且右孩子更小，则选择右孩子。
        if (right < m_heap.size() && m_heap[right].m_expire < m_heap[left].m_expire) smaller = right;

        // 已经满足最小堆性质。
        if (m_heap[index].m_expire <= m_heap[smaller].m_expire) break;

        // 当前节点较大，与较小孩子交换。
        swapNode(index, smaller);

        index = smaller;
    }
}

void TimerManager::swapNode(size_t lhs, size_t rhs)
{
    if (lhs == rhs) return;

    std::swap(m_heap[lhs], m_heap[rhs]);

    // 现在两个结点已经交换，新结点的下标就是原节点的下标。
    m_timerIndex[m_heap[lhs].m_id] = lhs;
    m_timerIndex[m_heap[rhs].m_id] = rhs;
}

void TimerManager::remove(size_t index)
{
    // 为了维持完全二叉树的性质，我们把数组末端的值覆盖到需要删除的位置，然后删除数组末端的元素，最后调整这棵树即可。
    if (index >= m_heap.size()) return;

    swapNode(index, m_heap.size() - 1);

    m_timerIndex.erase(m_heap.back().m_id);
    m_heap.pop_back();

    // 如果删除的是末尾元素，那么删除以后 index 没有意义，最小堆仍然成立，应该直接返回。
    if (index >= m_heap.size()) return;

    // 替换后的节点可能破坏堆性质。当前结点的值可能会变得更大或者更小。若变得更小，对子树没有影响，只用考虑和父结点的关系，对应 heapifyUp()。若变得更大，则与父节点的关系一定不会破坏，只需要检查子树，对应 heapifyDown()。其他情况则满足条件。
    // index > 0 是因为根结点的父节点是没有意义的。若删除根节点，会走到 heapifyDown() 分支，从语义上讲是正确的，因为根节点的值最小，只可能变大影响和子树的关系。
    if (index > 0 && m_heap[index].m_expire < m_heap[getParentIndex(index)].m_expire)
    {
        heapifyUp(index);
    }
    // 否则可能需要向下调整。
    else
    {
        heapifyDown(index);
    }
}

void TimerManager::pop()
{
    // 采用相同的思路，为了维持完全二叉树的性质，我们把数组末端的值覆盖到数组首部，然后删除数组末端的元素，最后使用 heapifyDown() 调整即可。这里复用了 remove() 代码。
    // 删除堆顶元素后，将最后一个节点移动到根节点，此时可能不满足最小堆性质。由于问题只可能出现在新根节点与子节点之间，因此从根节点开始向下调整。每次选择左右孩子中较小的节点与当前节点交换，这样能保证交换后的根结点是三者中最小的。交换后的子节点值变大了，它和它的子树可能不符合要求，持续调整满足最小堆要求。
    remove(0);
}
