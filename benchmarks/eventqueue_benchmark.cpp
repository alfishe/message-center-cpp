#include "eventqueue_benchmark.h"

#include <benchmark/benchmark.h>
#include "eventqueue.h"
#include <string>
#include <sstream>

using namespace std;

static void generateTopics(string topics[])
{
    for (int i = 0; i < MAX_TOPICS; i++)
    {
        stringstream ss;
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
    srand(time(NULL));
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

    EventQueueCUT queue;
    for (int i = 0; i < MAX_TOPICS; i++)
    {
        queue.RegisterTopic(topics[i]);
    }

    for (auto _ : state)
    {
        for (int i = 0; i < state.range(0); i++)
        {
            queue.ResolveTopic(topics[i % MAX_TOPICS]);
        }
    }

    state.SetComplexityN(state.range(0));
}

BENCHMARK(BM_ResolveNonExistingTopics)->RangeMultiplier(10)->Range(1, 10000)->Complexity(benchmark::oN);