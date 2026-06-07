#include <benchmark/benchmark.h>
#include "Orderbook.h"
#include <random>
#include <vector>
#include <algorithm>

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
    
    // We recreate the orderbook outside the timer loop
    for (auto _ : state) {
        state.PauseTiming();
        Orderbook orderbook;
        OrderId id = 0;
        state.ResumeTiming();

        for (const auto& action : actions) {
            orderbook.AddOrder(OrderType::GoodTillCancel, ++id, action.side, action.price, action.qty);
        }
        benchmark::DoNotOptimize(orderbook);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_AddOrders)->RangeMultiplier(10)->Range(1000, 100000)->Unit(benchmark::kMillisecond);

// ── BM_CancelOrders ───────────────────────────────────────────────────────

static void BM_CancelOrders(benchmark::State& state) {
    size_t numOrders = state.range(0);
    auto actions = GenerateRandomOrders(numOrders);

    for (auto _ : state) {
        state.PauseTiming();
        Orderbook orderbook;
        OrderId id = 0;
        std::vector<OrderId> activeIds;
        activeIds.reserve(numOrders);
        
        for (const auto& action : actions) {
            OrderId oid = ++id;
            orderbook.AddOrder(OrderType::GoodTillCancel, oid, action.side, action.price, action.qty);
            if (orderbook.HasOrder(oid)) activeIds.push_back(oid);
        }
        state.ResumeTiming();

        // Time the cancellations
        for (OrderId oid : activeIds) {
            orderbook.CancelOrder(oid);
        }
        benchmark::DoNotOptimize(orderbook);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_CancelOrders)->RangeMultiplier(10)->Range(1000, 100000)->Unit(benchmark::kMicrosecond);

// ── BM_MatchOrders ────────────────────────────────────────────────────────

static void BM_MatchOrders(benchmark::State& state) {
    size_t numOrders = state.range(0);
    
    // Build a resting book
    std::mt19937 gen(42);
    std::uniform_int_distribution<Quantity> qtyDist(1, 100);

    for (auto _ : state) {
        state.PauseTiming();
        Orderbook orderbook;
        OrderId id = 0;
        
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
            orderbook.AddOrder(OrderType::GoodTillCancel, ++id, Side::Sell, 100, q);
        }
        benchmark::DoNotOptimize(orderbook);
    }
    state.SetItemsProcessed(state.iterations() * (state.range(0) / 2));
}
BENCHMARK(BM_MatchOrders)->RangeMultiplier(10)->Range(1000, 100000)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
