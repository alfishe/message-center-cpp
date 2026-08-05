// EventQueueBroadcastFast Example
// High-performance lock-free broadcast queue
//
// Build: mkdir build && cd build && cmake .. && make
// Run:   ./fast_queue_example

#include "../../src/eventqueue_fast.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

struct Position { float x, y, z; };

void onMove(uint16_t topicId, const void* data, size_t size, void* userData) {
    const Position* p = static_cast<const Position*>(data);
    printf("[Move] (%.1f, %.1f, %.1f)\n", p->x, p->y, p->z);
}

void onHealth(uint16_t topicId, const void* data, size_t size, void* userData) {
    printf("[Health] %d HP\n", *static_cast<const int*>(data));
}

void onChat(uint16_t topicId, const void* data, size_t size, void* userData) {
    printf("[Chat] %.*s\n", static_cast<int>(size), static_cast<const char*>(data));
}

int main() {
    printf("=== EventQueueBroadcastFast Example ===\n\n");

    EventQueueBroadcastFast<4096> queue;

    uint16_t moveTopic = queue.registerTopic();
    uint16_t healthTopic = queue.registerTopic();
    uint16_t chatTopic = queue.registerTopic();

    queue.addObserver(moveTopic, onMove, nullptr);
    queue.addObserver(healthTopic, onHealth, nullptr);
    queue.addObserver(chatTopic, onChat, nullptr);

    // Post events
    Position pos = {10.0f, 20.0f, 5.0f};
    queue.post(moveTopic, &pos, sizeof(pos));

    int health = 85;
    queue.post(healthTopic, &health, sizeof(health));

    const char* msg = "Hello!";
    queue.post(chatTopic, msg, strlen(msg));

    queue.dispatchAll();

    // Multi-threaded
    printf("\n--- Multi-threaded ---\n");

    std::atomic<bool> running{true};
    std::atomic<int> count{0};

    std::thread producer([&]() {
        for (int i = 0; i < 100; i++) {
            Position p = {static_cast<float>(i), 0.0f, 0.0f};
            queue.post(moveTopic, &p, sizeof(p));
            count++;
        }
    });

    std::thread consumer([&]() {
        while (running || !queue.empty()) {
            if (!queue.dispatchOne()) std::this_thread::yield();
        }
    });

    producer.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    running = false;
    consumer.join();

    printf("Posted: %d\n", count.load());
    return 0;
}
