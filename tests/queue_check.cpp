#include "recorder.hpp"
#include <future>
#include <iostream>

using namespace std::chrono_literals;

int main()
{
    capture::BoundedQueue<int> queue(1);
    if (!queue.push(42)) return 1;
    auto blocked = std::async(std::launch::async, [&] { return queue.push(43); });
    if (blocked.wait_for(30ms) != std::future_status::timeout) return 2;
    queue.close();
    if (blocked.wait_for(1s) != std::future_status::ready || blocked.get()) return 3;
    if (queue.pop() != 42 || queue.pop().has_value() || queue.peak() != 1) return 4;
    capture::BoundedQueue<int> empty(1);
    auto consumer = std::async(std::launch::async, [&] { return empty.pop(); });
    empty.close();
    if (consumer.wait_for(1s) != std::future_status::ready || consumer.get()) return 5;
    std::cout << "PASS close wakes producers and consumers; accepted records drain\n";
}
