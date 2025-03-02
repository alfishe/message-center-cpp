#include <benchmark/benchmark.h>

#include "eventqueue.h"
#include "eventqueue_benchmark.h"
#include <string>
#include <sstream>

using namespace std;

static void generateTopics(string topics[], string prefix = "")
{
    for (int i = 0; i < MAX_TOPICS; i++)
    {
        stringstream ss;
        if (!prefix.empty())
            ss << prefix << "_";
        ss << "topic_" << i;
        topics[i] = ss.str();
    }
}

static void BM_RegisterTopics(benchmark::State& state)
{
    string topics[MAX_TOPICS];
    generateTopics(topics);

    EventQueueCUT queue;

    for (auto _ : state)
    {
        for (int i = 0; i < state.range(0); i++)
        {
            queue.RegisterTopic(topics[i]);
        }

        queue.ClearTopics();
    }

    state.SetComplexityN(state.range(0));
}

BENCHMARK(BM_RegisterTopics)->RangeMultiplier(10)->Range(1, 1000)->Complexity(benchmark::oN);

static void BM_ResolveExistingTopics(benchmark::State& state)
{
    srand((unsigned)time(NULL));
    string topics[MAX_TOPICS];
    generateTopics(topics);

    EventQueueCUT queue;

    for (auto _ : state)
    {
        state.PauseTiming();
        for (int i = 0; i < state.range(0) % MAX_TOPICS; i++)
        {
            queue.RegisterTopic(topics[i]);
        }

        int randomIndex = rand() % state.range(0);
        state.ResumeTiming();

        queue.ResolveTopic(topics[randomIndex % MAX_TOPICS]);

        state.PauseTiming();
        queue.ClearTopics();
        state.ResumeTiming();
    }

    state.SetComplexityN(state.range(0));
}

BENCHMARK(BM_ResolveExistingTopics)->RangeMultiplier(10)->Range(1, 10000)->Iterations(100)->Complexity(benchmark::oN);

static void BM_ResolveNonExistingTopics(benchmark::State& state)
{
    string topics[MAX_TOPICS];
    generateTopics(topics);

    string keys[MAX_TOPICS];
    generateTopics(keys, "key");

    EventQueueCUT queue;
    for (int i = 0; i < MAX_TOPICS; i++)
    {
        queue.RegisterTopic(topics[i]);
    }

    for (auto _ : state)
    {
        state.PauseTiming();
        for (int i = 0; i < state.range(0) % MAX_TOPICS; i++)
        {
            queue.RegisterTopic(topics[i]);
        }

        int randomIndex = rand() % state.range(0);
        state.ResumeTiming();

        queue.ResolveTopic(keys[randomIndex % MAX_TOPICS]);

        state.PauseTiming();
        queue.ClearTopics();
        state.ResumeTiming();
    }

    state.SetComplexityN(state.range(0));
}

BENCHMARK(BM_ResolveNonExistingTopics)->RangeMultiplier(10)->Range(1, 10000)->Iterations(100)->Complexity(benchmark::oN);

static void BM_PostSingleTopic(benchmark::State& state)
{
    EventQueueCUT queue;
    FillQueue(queue);

    for (auto _ : state)
    {
        for (int i = 0; i < state.range(0); i++)
        {
            queue.Post(1, nullptr);
        }

        state.SetComplexityN(state.range(0));
    }
}

BENCHMARK(BM_PostSingleTopic)->RangeMultiplier(10)->Range(1, 10000)->Complexity(benchmark::oN);

static void BM_GetMessageSingleTopic(benchmark::State& state)
{
    EventQueueCUT queue;
    FillQueue(queue);

    for (auto _ : state)
    {
        state.PauseTiming();
        for (int i = 0; i < state.range(0); i++)
        {
            queue.Post(1, nullptr);
        }
        state.ResumeTiming();

        for (int i = 0; i < state.range(0); i++)
        {
            Message* message = queue.GetQueueMessage();
            if (message->tid)
            {
                ;
            }
        }

        state.SetComplexityN(state.range(0));
    }
}

BENCHMARK(BM_GetMessageSingleTopic)->RangeMultiplier(10)->Range(1, 10000)->Complexity(benchmark::oN);

/// region <TPS benchmarks>
static void BM_PostSingleTopicTPS(benchmark::State& state)
{
    EventQueueCUT queue;
    FillQueue(queue);

    for (auto _ : state)
    {
        for (int i = 0; i < state.range(0); i++)
        {
            queue.Post(1, nullptr);
        }
    }

    // Calculate transactions per period and report it to the benchmark framework
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_PostSingleTopicTPS)->Range(1, 1 << 18)->Unit(benchmark::kMillisecond);

static void BM_GetMessageSingleTopicTPS(benchmark::State& state)
{
    EventQueueCUT queue;
    FillQueue(queue);

    for (auto _ : state)
    {
        state.PauseTiming();
        for (int i = 0; i < state.range(0); i++)
        {
            queue.Post(1, nullptr);
        }
        state.ResumeTiming();

        for (int i = 0; i < state.range(0); i++)
        {
            Message* message = queue.GetQueueMessage();
            if (message->tid)
            {
                ;
            }
        }
    }

    // Calculate transactions per period and report it to the benchmark framework
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_GetMessageSingleTopicTPS)->Range(1, 1 << 18)->Unit(benchmark::kMillisecond);

/// region <Multi-threaded TPS benchmarks>

static void BM_MT_PostSingleTopicTPS(benchmark::State& state)
{
    EventQueueCUT queue;
    FillQueue(queue);

    for (auto _ : state)
    {
        for (int i = 0; i < state.range(0); i++)
        {
            queue.Post(1, nullptr);
        }
    }

    // Calculate transactions per period and report it to the benchmark framework
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_MT_PostSingleTopicTPS)->Range(1, 1 << 18)->Unit(benchmark::kMillisecond)->ThreadRange(1, std::thread::hardware_concurrency());

static void BM_MT_GetMessageSingleTopicTPS(benchmark::State& state)
{
    EventQueueCUT queue;
    FillQueue(queue);

    for (auto _ : state)
    {
        state.PauseTiming();
        for (int i = 0; i < state.range(0); i++)
        {
            queue.Post(1, nullptr);
        }
        state.ResumeTiming();

        for (int i = 0; i < state.range(0); i++)
        {
            Message* message = queue.GetQueueMessage();
            if (message->tid)
            {
                ;
            }
        }
    }

    // Calculate transactions per period and report it to the benchmark framework
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_GetMessageSingleTopicTPS)->Range(1, 1 << 18)->Unit(benchmark::kMillisecond)->ThreadRange(1, std::thread::hardware_concurrency());

/// endregion </Multi-threaded TPS benchmarks>

/// endregion </TPS benchmarks>