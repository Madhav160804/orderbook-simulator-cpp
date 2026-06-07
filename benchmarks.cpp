#include <benchmark/benchmark.h>
#include "Orderbook.h"
#include <random>
#include <vector>
#include <algorithm>
#include <chrono>
#include <numeric>

// Pre-generate RNG data to eliminate overhead from the timer loop
struct AddAction {
    Side side;
    Price price;
    Quantity qty;
};

std::vector<AddAction> GenerateRandomOrders(size_t n, int seed = 42) {
    std::mt19937 gen(seed);
    std::uniform_int_distribution<Price>    priceDist(90, 110);
    std::uniform_int_distribution<Quantity> qtyDist(1, 100);
    std::uniform_int_distribution<int>      sideDist(0, 1);

    std::vector<AddAction> actions;
    actions.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        actions.push_back({
            sideDist(gen) ? Side::Buy : Side::Sell,
            priceDist(gen),
            qtyDist(gen)
        });
    }
    return actions;
}

// ── BM_AddOrders ──────────────────────────────────────────────────────────

static void BM_AddOrders(benchmark::State& state) {
    auto actions = GenerateRandomOrders(state.range(0));
    
    double total_p99 = 0.0;
    double total_avg = 0.0;

    for (auto _ : state) {
        state.PauseTiming();
        Orderbook orderbook;
        OrderId id = 0;
        std::vector<double> latencies;
        latencies.reserve(state.range(0));
        state.ResumeTiming();

        for (const auto& action : actions) {
            auto t1 = std::chrono::high_resolution_clock::now();
            orderbook.AddOrder(OrderType::GoodTillCancel, ++id, action.side, action.price, action.qty);
            auto t2 = std::chrono::high_resolution_clock::now();
            latencies.push_back(std::chrono::duration<double, std::nano>(t2 - t1).count());
        }
        benchmark::DoNotOptimize(orderbook);

        state.PauseTiming();
        std::sort(latencies.begin(), latencies.end());
        double p99 = latencies[static_cast<size_t>(latencies.size() * 0.99)];
        double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);
        double avg = sum / latencies.size();
        total_p99 += p99;
        total_avg += avg;
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    state.counters["Avg_ns"] = total_avg / state.iterations();
    state.counters["P99_ns"] = total_p99 / state.iterations();
}
BENCHMARK(BM_AddOrders)->RangeMultiplier(10)->Range(1000, 100000)->Unit(benchmark::kMillisecond);

// ── BM_CancelOrders ───────────────────────────────────────────────────────

static void BM_CancelOrders(benchmark::State& state) {
    size_t numOrders = state.range(0);
    auto actions = GenerateRandomOrders(numOrders);

    double total_p99 = 0.0;
    double total_avg = 0.0;

    for (auto _ : state) {
        state.PauseTiming();
        Orderbook orderbook;
        OrderId id = 0;
        std::vector<OrderId> activeIds;
        activeIds.reserve(numOrders);
        std::vector<double> latencies;
        latencies.reserve(numOrders);
        
        for (const auto& action : actions) {
            OrderId oid = ++id;
            orderbook.AddOrder(OrderType::GoodTillCancel, oid, action.side, action.price, action.qty);
            if (orderbook.HasOrder(oid)) activeIds.push_back(oid);
        }
        state.ResumeTiming();

        // Time the cancellations
        for (OrderId oid : activeIds) {
            auto t1 = std::chrono::high_resolution_clock::now();
            orderbook.CancelOrder(oid);
            auto t2 = std::chrono::high_resolution_clock::now();
            latencies.push_back(std::chrono::duration<double, std::nano>(t2 - t1).count());
        }
        benchmark::DoNotOptimize(orderbook);

        state.PauseTiming();
        if (!latencies.empty()) {
            std::sort(latencies.begin(), latencies.end());
            double p99 = latencies[static_cast<size_t>(latencies.size() * 0.99)];
            double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);
            double avg = sum / latencies.size();
            total_p99 += p99;
            total_avg += avg;
        }
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    state.counters["Avg_ns"] = total_avg / state.iterations();
    state.counters["P99_ns"] = total_p99 / state.iterations();
}
BENCHMARK(BM_CancelOrders)->RangeMultiplier(10)->Range(1000, 100000)->Unit(benchmark::kMicrosecond);

// ── BM_MatchOrders ────────────────────────────────────────────────────────

static void BM_MatchOrders(benchmark::State& state) {
    size_t numOrders = state.range(0);
    
    // Build a resting book
    std::mt19937 gen(42);
    std::uniform_int_distribution<Quantity> qtyDist(1, 100);

    double total_p99 = 0.0;
    double total_avg = 0.0;

    for (auto _ : state) {
        state.PauseTiming();
        Orderbook orderbook;
        OrderId id = 0;
        std::vector<double> latencies;
        latencies.reserve(numOrders / 2);
        
        // Add passive buys
        for (size_t i = 0; i < numOrders / 2; ++i) {
            orderbook.AddOrder(OrderType::GoodTillCancel, ++id, Side::Buy, 100, qtyDist(gen));
        }

        // Pre-generate aggressive sells
        std::vector<Quantity> aggressiveQts;
        aggressiveQts.reserve(numOrders / 2);
        for (size_t i = 0; i < numOrders / 2; ++i) {
            aggressiveQts.push_back(qtyDist(gen));
        }
        state.ResumeTiming();

        for (Quantity q : aggressiveQts) {
            auto t1 = std::chrono::high_resolution_clock::now();
            orderbook.AddOrder(OrderType::GoodTillCancel, ++id, Side::Sell, 100, q);
            auto t2 = std::chrono::high_resolution_clock::now();
            latencies.push_back(std::chrono::duration<double, std::nano>(t2 - t1).count());
        }
        benchmark::DoNotOptimize(orderbook);

        state.PauseTiming();
        if (!latencies.empty()) {
            std::sort(latencies.begin(), latencies.end());
            double p99 = latencies[static_cast<size_t>(latencies.size() * 0.99)];
            double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);
            double avg = sum / latencies.size();
            total_p99 += p99;
            total_avg += avg;
        }
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * (state.range(0) / 2));
    state.counters["Avg_ns"] = total_avg / state.iterations();
    state.counters["P99_ns"] = total_p99 / state.iterations();
}
BENCHMARK(BM_MatchOrders)->RangeMultiplier(10)->Range(1000, 100000)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
