// MessageCenter Example
// Demonstrates the high-level singleton API with automatic dispatch

#include "../../src/messagecenter.h"
#include <cstdio>
#include <thread>
#include <chrono>

// Define typed payloads
struct PlayerMoved : public MessagePayload {
    uint32_t playerId;
    float x, y;
    PlayerMoved(uint32_t id, float x, float y) : playerId(id), x(x), y(y) {}
};

struct PlayerHealth : public MessagePayload {
    uint32_t playerId;
    int health;
    int maxHealth;
    PlayerHealth(uint32_t id, int hp, int max) : playerId(id), health(hp), maxHealth(max) {}
};

struct GameEvent : public MessagePayload {
    const char* eventName;
    GameEvent(const char* name) : eventName(name) {}
};

// Observer class example - must derive from Observer base
class GameLogger : public Observer {
public:
    void onPlayerMoved(int id, Message* msg) {
        auto* e = static_cast<PlayerMoved*>(msg->obj);
        printf("[Logger] Player %u moved to (%.1f, %.1f)\n", e->playerId, e->x, e->y);
    }

    void onPlayerHealth(int id, Message* msg) {
        auto* e = static_cast<PlayerHealth*>(msg->obj);
        printf("[Logger] Player %u health: %d/%d\n", e->playerId, e->health, e->maxHealth);
    }
};

// Helper typedef for method pointers
typedef void (Observer::*ObserverMethod)(int, Message*);

int main() {
    printf("=== MessageCenter Example ===\n\n");

    // Get singleton instance (auto-starts dispatcher thread)
    auto& mc = MessageCenter::DefaultMessageCenter();

    // Create observer instance
    GameLogger logger;

    // Subscribe with class method (cast required for derived class methods)
    mc.AddObserver("player.moved", &logger, (ObserverMethod)&GameLogger::onPlayerMoved);
    mc.AddObserver("player.health", &logger, (ObserverMethod)&GameLogger::onPlayerHealth);

    // Subscribe with lambda - logging (explicit std::function to avoid ambiguity)
    mc.AddObserver("game.event", ObserverCallbackFunc([](int id, Message* msg) {
        auto* e = static_cast<GameEvent*>(msg->obj);
        printf("[Event] %s\n", e->eventName);
    }));

    // Subscribe with lambda - boundary checker
    float maxX = 100.0f, maxY = 100.0f;
    mc.AddObserver("player.moved", ObserverCallbackFunc([maxX, maxY](int id, Message* msg) {
        auto* e = static_cast<PlayerMoved*>(msg->obj);
        if (e->x > maxX || e->y > maxY) {
            printf("[Bounds] Player %u out of bounds!\n", e->playerId);
        }
    }));

    // Subscribe with lambda - health warning
    mc.AddObserver("player.health", ObserverCallbackFunc([](int id, Message* msg) {
        auto* e = static_cast<PlayerHealth*>(msg->obj);
        if (e->health < e->maxHealth / 4) {
            printf("[Warning] Player %u health critical!\n", e->playerId);
        }
    }));

    printf("--- Publishing events ---\n\n");

    // Post events (fire-and-forget, auto-cleaned after dispatch)
    mc.Post("game.event", new GameEvent("Game started"));
    mc.Post("player.moved", new PlayerMoved(1, 10.5f, 20.0f));
    mc.Post("player.moved", new PlayerMoved(2, 150.0f, 50.0f));  // Out of bounds
    mc.Post("player.health", new PlayerHealth(1, 100, 100));
    mc.Post("player.health", new PlayerHealth(2, 15, 100));      // Critical
    mc.Post("game.event", new GameEvent("Round complete"));

    // Wait for dispatcher thread to process
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    printf("\n--- Cleanup ---\n");
    MessageCenter::DisposeDefaultMessageCenter();
    printf("MessageCenter disposed.\n");

    return 0;
}
