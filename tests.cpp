#include <iostream>
#include <cassert>
#include <chrono>
#include <random>
#include <iomanip>
#include "Orderbook.h"
#include "Types.h"
#include "Ordertype.h"

// Test helper macros
#define TEST(name) void name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    name(); \
    std::cout << "PASSED\n"; \
} while(0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))

// Helper function to format large numbers with commas
std::string formatNumber(long long num) {
    std::string str = std::to_string(num);
    int insertPosition = str.length() - 3;
    while (insertPosition > 0) {
        str.insert(insertPosition, ",");
        insertPosition -= 3;
    }
    return str;
}

// ==================== FUNCTIONALITY TESTS ====================

TEST(TestBasicAddOrder) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Buy, 100, 10);
    ASSERT_EQ(orderbook.Size(), 1);
}

TEST(TestCancelOrder) {
    Orderbook orderbook;
    OrderId orderId = 1;
    orderbook.AddOrder(OrderType::GoodTillCancel, orderId, Side::Buy, 100, 10);
    ASSERT_EQ(orderbook.Size(), 1);
    orderbook.CancelOrder(orderId);
    ASSERT_EQ(orderbook.Size(), 0);
}

TEST(TestDuplicateOrderRejection) {
    Orderbook orderbook;
    OrderId orderId = 1;
    orderbook.AddOrder(OrderType::GoodTillCancel, orderId, Side::Buy, 100, 10);
    auto trades = orderbook.AddOrder(OrderType::GoodTillCancel, orderId, Side::Buy, 100, 10);
    ASSERT_EQ(orderbook.Size(), 1);
    ASSERT_TRUE(trades.empty());
}

TEST(TestSimpleMatch) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Buy, 100, 10);
    auto trades = orderbook.AddOrder(OrderType::GoodTillCancel, 2, Side::Sell, 100, 10);

    ASSERT_EQ(trades.size(), 1);
    ASSERT_EQ(orderbook.Size(), 0);
    ASSERT_EQ(trades[0].GetQuantity(), 10);
    ASSERT_EQ(trades[0].GetQuantity(), 10);
}

TEST(TestPartialMatch) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Buy, 100, 15);
    auto trades = orderbook.AddOrder(OrderType::GoodTillCancel, 2, Side::Sell, 100, 10);

    ASSERT_EQ(trades.size(), 1);
    ASSERT_EQ(orderbook.Size(), 1);
    ASSERT_EQ(trades[0].GetQuantity(), 10);
}

TEST(TestMultipleMatchesAtSamePrice) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Buy, 100, 5);
    orderbook.AddOrder(OrderType::GoodTillCancel, 2, Side::Buy, 100, 5);
    orderbook.AddOrder(OrderType::GoodTillCancel, 3, Side::Buy, 100, 5);

    auto trades = orderbook.AddOrder(OrderType::GoodTillCancel, 4, Side::Sell, 100, 12);

    ASSERT_EQ(trades.size(), 3);
    ASSERT_EQ(orderbook.Size(), 1);
}

TEST(TestPricePriority) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Buy, 100, 10);
    orderbook.AddOrder(OrderType::GoodTillCancel, 2, Side::Buy, 105, 10);

    auto trades = orderbook.AddOrder(OrderType::GoodTillCancel, 3, Side::Sell, 100, 10);

    ASSERT_EQ(trades.size(), 1);
    ASSERT_EQ(trades[0].GetBidOrderId(), 2);        // highest bid matched first
    ASSERT_EQ(trades[0].GetPrice(), 100);        // executed at ask price
}

TEST(TestTimePriority_FIFO) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Buy, 100, 10);
    orderbook.AddOrder(OrderType::GoodTillCancel, 2, Side::Buy, 100, 10);

    auto trades = orderbook.AddOrder(OrderType::GoodTillCancel, 3, Side::Sell, 100, 10);

    ASSERT_EQ(trades.size(), 1);
    ASSERT_EQ(trades[0].GetBidOrderId(), 1);        // order 1 arrived first
}

TEST(TestMarketOrderBuy) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Sell, 100, 10);

    auto trades = orderbook.AddOrder(OrderType::Market, 2, Side::Buy, 0, 10);

    ASSERT_EQ(trades.size(), 1);
    ASSERT_EQ(orderbook.Size(), 0);
}

TEST(TestMarketOrderSell) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Buy, 100, 10);

    auto trades = orderbook.AddOrder(OrderType::Market, 2, Side::Sell, 0, 10);

    ASSERT_EQ(trades.size(), 1);
    ASSERT_EQ(orderbook.Size(), 0);
}

TEST(TestMarketOrderEmptyBook) {
    Orderbook orderbook;
    auto trades = orderbook.AddOrder(OrderType::Market, 1, Side::Buy, 0, 10);

    ASSERT_TRUE(trades.empty());
    ASSERT_EQ(orderbook.Size(), 0);
}

TEST(TestImmediateOrCancel_PartialFill) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Sell, 100, 5);

    auto trades = orderbook.AddOrder(OrderType::ImmediateOrCancel, 2, Side::Buy, 100, 10);

    ASSERT_EQ(trades.size(), 1);
    ASSERT_EQ(trades[0].GetQuantity(), 5);
    ASSERT_EQ(orderbook.Size(), 0);
}

TEST(TestImmediateOrCancel_NoMatch) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Sell, 105, 10);

    auto trades = orderbook.AddOrder(OrderType::ImmediateOrCancel, 2, Side::Buy, 100, 10);

    ASSERT_TRUE(trades.empty());
    ASSERT_EQ(orderbook.Size(), 1);
}

TEST(TestFillOrKill_FullFill) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Sell, 100, 10);

    auto trades = orderbook.AddOrder(OrderType::FillOrKill, 2, Side::Buy, 100, 10);

    ASSERT_EQ(trades.size(), 1);
    ASSERT_EQ(trades[0].GetQuantity(), 10);
    ASSERT_EQ(orderbook.Size(), 0);
}

TEST(TestFillOrKill_PartialAvailable) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Sell, 100, 5);

    auto trades = orderbook.AddOrder(OrderType::FillOrKill, 2, Side::Buy, 100, 10);

    ASSERT_TRUE(trades.empty());
    ASSERT_EQ(orderbook.Size(), 1);
}

TEST(TestFillOrKill_MultipleOrders) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Sell, 100, 5);
    orderbook.AddOrder(OrderType::GoodTillCancel, 2, Side::Sell, 100, 5);

    auto trades = orderbook.AddOrder(OrderType::FillOrKill, 3, Side::Buy, 100, 10);

    ASSERT_EQ(trades.size(), 2);
    ASSERT_EQ(orderbook.Size(), 0);
}

TEST(TestOrderModify) {
    Orderbook orderbook;
    OrderId orderId = 1;
    orderbook.AddOrder(OrderType::GoodTillCancel, orderId, Side::Buy, 100, 10);

    OrderModify modify(orderId, Side::Buy, 105, 15);
    orderbook.ModifyOrder(modify);

    ASSERT_EQ(orderbook.Size(), 1);
    auto infos = orderbook.GetOrderInfos();
    ASSERT_EQ(infos.GetBids()[0].price_, 105);
    ASSERT_EQ(infos.GetBids()[0].quantity_, 15);
}

TEST(TestOrderbookLevelInfos) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Buy, 100, 10);
    orderbook.AddOrder(OrderType::GoodTillCancel, 2, Side::Buy, 100, 5);
    orderbook.AddOrder(OrderType::GoodTillCancel, 3, Side::Sell, 105, 20);

    auto infos = orderbook.GetOrderInfos();
    ASSERT_EQ(infos.GetBids().size(), 1);
    ASSERT_EQ(infos.GetBids()[0].quantity_, 15);
    ASSERT_EQ(infos.GetAsks().size(), 1);
    ASSERT_EQ(infos.GetAsks()[0].quantity_, 20);
}

TEST(TestExchangeRulesBasic) {
    Orderbook orderbook;
    ExchangeRules rules;
    rules.tickSize   = 5;   // prices must be multiples of 5
    rules.lotSize    = 10;  // quantities must be multiples of 10
    rules.minQuantity = 10;
    orderbook.SetExchangeRules(rules);

    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Buy, 100, 20); // valid
    ASSERT_EQ(orderbook.Size(), 1);

    orderbook.AddOrder(OrderType::GoodTillCancel, 2, Side::Buy, 103, 20); // bad tick
    ASSERT_EQ(orderbook.Size(), 1);

    orderbook.AddOrder(OrderType::GoodTillCancel, 3, Side::Buy, 100, 15); // bad lot
    ASSERT_EQ(orderbook.Size(), 1);

    orderbook.AddOrder(OrderType::GoodTillCancel, 4, Side::Buy, 100, 5);  // below min qty
    ASSERT_EQ(orderbook.Size(), 1);
}

TEST(TestMinNotionalValidation) {
    Orderbook orderbook;
    ExchangeRules rules;
    rules.minNotional = 1000; // minimum order value = $10
    orderbook.SetExchangeRules(rules);

    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Buy, 150, 10); // 1500 >= 1000 ✓
    ASSERT_EQ(orderbook.Size(), 1);

    orderbook.AddOrder(OrderType::GoodTillCancel, 2, Side::Buy, 50, 10);  // 500 < 1000 ✗
    ASSERT_EQ(orderbook.Size(), 1);
}

TEST(TestMarketOrderValidation) {
    Orderbook orderbook;
    ExchangeRules rules;
    rules.lotSize = 10;
    orderbook.SetExchangeRules(rules);
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Sell, 100, 50);

    auto trades = orderbook.AddOrder(OrderType::Market, 2, Side::Buy, 0, 20); // valid lot
    ASSERT_EQ(trades.size(), 1);

    auto noTrades = orderbook.AddOrder(OrderType::Market, 3, Side::Buy, 0, 15); // bad lot
    ASSERT_TRUE(noTrades.empty());
}

// ==================== BUG REGRESSION TESTS ====================

// Bug 1: Swap-and-pop violated FIFO when cancelling from the middle of a price level.
TEST(TestFIFO_CancelMiddle_PreservesOrder) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Buy, 100, 5); // A
    orderbook.AddOrder(OrderType::GoodTillCancel, 2, Side::Buy, 100, 5); // B
    orderbook.AddOrder(OrderType::GoodTillCancel, 3, Side::Buy, 100, 5); // C

    orderbook.CancelOrder(2); // cancel B (middle)
    ASSERT_EQ(orderbook.Size(), 2);

    auto trades = orderbook.AddOrder(OrderType::GoodTillCancel, 4, Side::Sell, 100, 5);
    ASSERT_EQ(trades.size(), 1);
    ASSERT_EQ(trades[0].GetBidOrderId(), 1); // A, not C

    auto trades2 = orderbook.AddOrder(OrderType::GoodTillCancel, 5, Side::Sell, 100, 5);
    ASSERT_EQ(trades2.size(), 1);
    ASSERT_EQ(trades2[0].GetBidOrderId(), 3); // C
}

TEST(TestFIFO_CancelFirst_PreservesOrder) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Buy, 100, 5); // A
    orderbook.AddOrder(OrderType::GoodTillCancel, 2, Side::Buy, 100, 5); // B

    orderbook.CancelOrder(1); // cancel front

    auto trades = orderbook.AddOrder(OrderType::GoodTillCancel, 3, Side::Sell, 100, 5);
    ASSERT_EQ(trades.size(), 1);
    ASSERT_EQ(trades[0].GetBidOrderId(), 2);
    ASSERT_EQ(orderbook.Size(), 0);
}

TEST(TestFIFO_MultipleCancels_PreservesOrder) {
    Orderbook orderbook;
    for (OrderId id = 1; id <= 5; ++id)
        orderbook.AddOrder(OrderType::GoodTillCancel, id, Side::Buy, 100, 5);

    orderbook.CancelOrder(2);
    orderbook.CancelOrder(4);
    ASSERT_EQ(orderbook.Size(), 3);

    // Remaining FIFO order: 1, 3, 5
    auto t1 = orderbook.AddOrder(OrderType::GoodTillCancel, 10, Side::Sell, 100, 5);
    ASSERT_EQ(t1[0].GetBidOrderId(), 1);

    auto t2 = orderbook.AddOrder(OrderType::GoodTillCancel, 11, Side::Sell, 100, 5);
    ASSERT_EQ(t2[0].GetBidOrderId(), 3);

    auto t3 = orderbook.AddOrder(OrderType::GoodTillCancel, 12, Side::Sell, 100, 5);
    ASSERT_EQ(t3[0].GetBidOrderId(), 5);

    ASSERT_EQ(orderbook.Size(), 0);
}

// Bug 2: Two market orders matching each other at INT_MIN (invalid price).
TEST(TestTwoMarketOrders_NoInvalidTrade) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Sell, 100, 5);

    // Market buy for 10: fills 5, remaining 5 stays as GTC@INT_MAX
    auto trades1 = orderbook.AddOrder(OrderType::Market, 2, Side::Buy, 0, 10);
    ASSERT_EQ(trades1.size(), 1);
    ASSERT_EQ(trades1[0].GetQuantity(), 5);
    ASSERT_EQ(orderbook.Size(), 1);

    // Market sell: converts to GTC@INT_MIN. INT_MAX vs INT_MIN — no valid price.
    auto trades2 = orderbook.AddOrder(OrderType::Market, 3, Side::Sell, 0, 5);
    ASSERT_TRUE(trades2.empty());
}

// Bug 3: ModifyOrder must reject side changes.
TEST(TestModify_CannotChangeSide) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Buy, 100, 10);

    OrderModify wrongSide(1, Side::Sell, 95, 10);
    auto trades = orderbook.ModifyOrder(wrongSide);

    ASSERT_TRUE(trades.empty());
    ASSERT_EQ(orderbook.Size(), 1);
    auto infos = orderbook.GetOrderInfos();
    ASSERT_EQ(infos.GetBids()[0].price_, 100);
}

TEST(TestModify_SameSide_Works) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Buy, 100, 10);

    OrderModify valid(1, Side::Buy, 105, 20);
    orderbook.ModifyOrder(valid);

    ASSERT_EQ(orderbook.Size(), 1);
    auto infos = orderbook.GetOrderInfos();
    ASSERT_EQ(infos.GetBids()[0].price_, 105);
    ASSERT_EQ(infos.GetBids()[0].quantity_, 20);
}

// Quantity-only modify must preserve time priority (DLL position unchanged).
TEST(TestModify_QuantityOnly_PreservesTimePriority) {
    Orderbook orderbook;
    orderbook.AddOrder(OrderType::GoodTillCancel, 1, Side::Buy, 100, 10);
    orderbook.AddOrder(OrderType::GoodTillCancel, 2, Side::Buy, 100, 10);
    orderbook.AddOrder(OrderType::GoodTillCancel, 3, Side::Buy, 100, 10);

    // Modify order 2 qty only — stays in middle of DLL
    OrderModify qtyOnly(2, Side::Buy, 100, 50);
    orderbook.ModifyOrder(qtyOnly);
    ASSERT_EQ(orderbook.Size(), 3);

    // Sell 15: fills order 1 (10) fully, then 5 from order 2 — not order 3
    auto trades = orderbook.AddOrder(OrderType::GoodTillCancel, 4, Side::Sell, 100, 15);
    ASSERT_EQ(trades.size(), 2);
    ASSERT_EQ(trades[0].GetBidOrderId(), 1);
    ASSERT_EQ(trades[0].GetQuantity(), 10);
    ASSERT_EQ(trades[1].GetBidOrderId(), 2);
    ASSERT_EQ(trades[1].GetQuantity(), 5);

    auto infos = orderbook.GetOrderInfos();
    ASSERT_EQ(infos.GetBids()[0].quantity_, 45 + 10); // 45 remaining from 2 + 10 from 3
}

// ==================== PERFORMANCE BENCHMARKS ====================

void PrintPerformanceHeader() {
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << std::setw(45) << "PERFORMANCE BENCHMARKS\n";
    std::cout << std::string(70, '=') << "\n\n";
}

void BenchmarkAddOrders(int numOrders) {
    Orderbook orderbook;
    std::mt19937 gen(42);
    std::uniform_int_distribution<Price>    priceDist(90, 110);
    std::uniform_int_distribution<Quantity> qtyDist(1, 100);
    std::uniform_int_distribution<int>      sideDist(0, 1);

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < numOrders; ++i) {
        orderbook.AddOrder(
            OrderType::GoodTillCancel, i,
            sideDist(gen) ? Side::Buy : Side::Sell,
            priceDist(gen), qtyDist(gen)
        );
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double seconds = duration.count() / 1e6;
    std::cout << "Add " << formatNumber(numOrders) << " orders:\n";
    std::cout << "  Time: " << std::fixed << std::setprecision(2) << duration.count() / 1000.0 << " ms\n";
    std::cout << "  Throughput: " << formatNumber((long long)(numOrders / seconds)) << " orders/sec\n";
    std::cout << "  Latency: " << std::setprecision(3) << duration.count() / (double)numOrders << " μs/order\n\n";
}

void BenchmarkMatching(int numOrders) {
    Orderbook orderbook;
    std::mt19937 gen(42);
    std::uniform_int_distribution<Quantity> qtyDist(1, 100);

    for (int i = 0; i < numOrders / 2; ++i)
        orderbook.AddOrder(OrderType::GoodTillCancel, i, Side::Buy, 100, qtyDist(gen));

    int tradesExecuted = 0;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = numOrders / 2; i < numOrders; ++i) {
        auto trades = orderbook.AddOrder(OrderType::GoodTillCancel, i, Side::Sell, 100, qtyDist(gen));
        tradesExecuted += trades.size();
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double seconds = duration.count() / 1e6;
    std::cout << "Match " << formatNumber(numOrders / 2) << " orders:\n";
    std::cout << "  Time: " << std::fixed << std::setprecision(2) << duration.count() / 1000.0 << " ms\n";
    std::cout << "  Trades executed: " << formatNumber(tradesExecuted) << "\n";
    std::cout << "  Throughput: " << formatNumber((long long)((numOrders / 2) / seconds)) << " matches/sec\n";
    std::cout << "  Trade rate: " << formatNumber((long long)(tradesExecuted / seconds)) << " trades/sec\n\n";
}

void BenchmarkCancelOrders(int numOrders) {
    Orderbook orderbook;
    std::vector<OrderId> orderIds;
    for (int i = 0; i < numOrders; ++i) {
        orderbook.AddOrder(OrderType::GoodTillCancel, i, Side::Buy, 100, 10);
        orderIds.push_back(i);
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (auto orderId : orderIds) orderbook.CancelOrder(orderId);
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double seconds = duration.count() / 1e6;
    std::cout << "Cancel " << formatNumber(numOrders) << " orders:\n";
    std::cout << "  Time: " << std::fixed << std::setprecision(2) << duration.count() / 1000.0 << " ms\n";
    std::cout << "  Throughput: " << formatNumber((long long)(numOrders / seconds)) << " cancels/sec\n";
    std::cout << "  Latency: " << std::setprecision(3) << duration.count() / (double)numOrders << " μs/cancel\n\n";
}

void BenchmarkModifyOrders(int numOrders) {
    Orderbook orderbook;
    std::mt19937 gen(42);
    std::uniform_int_distribution<Price>    priceDist(95, 105);
    std::uniform_int_distribution<Quantity> qtyDist(1, 100);

    std::vector<OrderId> orderIds;
    for (int i = 0; i < numOrders; ++i) {
        orderbook.AddOrder(OrderType::GoodTillCancel, i, Side::Buy, 100, 10);
        orderIds.push_back(i);
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (auto orderId : orderIds) {
        OrderModify modify(orderId, Side::Buy, priceDist(gen), qtyDist(gen));
        orderbook.ModifyOrder(modify);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double seconds = duration.count() / 1e6;
    std::cout << "Modify " << formatNumber(numOrders) << " orders:\n";
    std::cout << "  Time: " << std::fixed << std::setprecision(2) << duration.count() / 1000.0 << " ms\n";
    std::cout << "  Throughput: " << formatNumber((long long)(numOrders / seconds)) << " modifies/sec\n";
    std::cout << "  Latency: " << std::setprecision(3) << duration.count() / (double)numOrders << " μs/modify\n\n";
}

void BenchmarkGetOrderInfos(int numOrders, int numCalls) {
    Orderbook orderbook;
    for (int i = 0; i < numOrders; ++i)
        orderbook.AddOrder(OrderType::GoodTillCancel, i, Side::Buy, 100 + (i % 10), 10);

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < numCalls; ++i) { auto infos = orderbook.GetOrderInfos(); (void)infos; }
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double seconds = duration.count() / 1e6;
    std::cout << "GetOrderInfos (" << formatNumber(numOrders) << " orders, "
              << formatNumber(numCalls) << " calls):\n";
    std::cout << "  Time: " << std::fixed << std::setprecision(2) << duration.count() / 1000.0 << " ms\n";
    std::cout << "  Throughput: " << formatNumber((long long)(numCalls / seconds)) << " snapshots/sec\n";
    std::cout << "  Latency: " << std::setprecision(3) << duration.count() / (double)numCalls << " μs/snapshot\n\n";
}

// HFT simulation: large resting book, steady-state churn
void BenchmarkHighFrequencyTrading() {
    Orderbook orderbook;
    std::mt19937 gen(42);

    // Wide spread: buys ≤95, sells ≥105 — orders don't cross each other
    std::uniform_int_distribution<Price>    bidPriceDist(85, 95);
    std::uniform_int_distribution<Price>    askPriceDist(105, 115);
    std::uniform_int_distribution<Quantity> qtyDist(1, 10);
    std::uniform_int_distribution<int>      actionDist(0, 9);

    const int warmupOrders  = 10000;
    const int numOperations = 100000;
    std::vector<OrderId> activeOrders;
    activeOrders.reserve(warmupOrders + numOperations);
    OrderId nextOrderId = 0;

    // Warmup: build large resting book (not measured)
    for (int i = 0; i < warmupOrders; ++i) {
        Side side = (i % 2) ? Side::Buy : Side::Sell;
        Price price = (side == Side::Buy) ? bidPriceDist(gen) : askPriceDist(gen);
        OrderId oid = nextOrderId++;
        orderbook.AddOrder(OrderType::GoodTillCancel, oid, side, price, qtyDist(gen));
        if (orderbook.HasOrder(oid)) activeOrders.push_back(oid);
    }

    int addCount = 0, cancelCount = 0, modifyCount = 0, tradeCount = 0;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < numOperations; ++i) {
        int action = activeOrders.empty() ? 0 : actionDist(gen);

        if (action <= 4 || activeOrders.empty()) {
            // 50%: add new resting order
            Side side = (nextOrderId % 2) ? Side::Buy : Side::Sell;
            Price price = (side == Side::Buy) ? bidPriceDist(gen) : askPriceDist(gen);
            OrderId oid = nextOrderId++;
            auto trades = orderbook.AddOrder(OrderType::GoodTillCancel, oid, side, price, qtyDist(gen));
            addCount++;
            tradeCount += trades.size();
            if (orderbook.HasOrder(oid)) activeOrders.push_back(oid);
        } else if (action <= 7) {
            // 30%: cancel
            size_t idx = gen() % activeOrders.size();
            orderbook.CancelOrder(activeOrders[idx]);
            activeOrders.erase(activeOrders.begin() + idx);
            cancelCount++;
        } else {
            // 20%: modify
            size_t idx = gen() % activeOrders.size();
            Price price = (nextOrderId % 2) ? bidPriceDist(gen) : askPriceDist(gen);
            OrderModify modify(activeOrders[idx], Side::Buy, price, qtyDist(gen));
            orderbook.ModifyOrder(modify);
            modifyCount++;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    double seconds = duration.count() / 1000.0;
    std::cout << "High-Frequency Trading Simulation:\n";
    std::cout << "  Warmup book size: " << formatNumber(warmupOrders) << " orders\n";
    std::cout << "  Operations: " << formatNumber(numOperations)
              << " (Add: " << formatNumber(addCount)
              << ", Cancel: " << formatNumber(cancelCount)
              << ", Modify: " << formatNumber(modifyCount) << ")\n";
    std::cout << "  Trades executed: " << formatNumber(tradeCount) << "\n";
    std::cout << "  Time: " << std::fixed << std::setprecision(2) << duration.count() << " ms\n";
    std::cout << "  Throughput: " << formatNumber((long long)(numOperations / seconds)) << " operations/sec\n";
    std::cout << "  Final book size: " << formatNumber((long long)orderbook.Size()) << " orders\n\n";
}

// ==================== MAIN TEST RUNNER ====================

int main() {
    std::cout << std::string(70, '=') << "\n";
    std::cout << std::setw(45) << "ORDERBOOK FUNCTIONALITY TESTS\n";
    std::cout << std::string(70, '=') << "\n\n";

    RUN_TEST(TestBasicAddOrder);
    RUN_TEST(TestCancelOrder);
    RUN_TEST(TestDuplicateOrderRejection);
    RUN_TEST(TestSimpleMatch);
    RUN_TEST(TestPartialMatch);
    RUN_TEST(TestMultipleMatchesAtSamePrice);
    RUN_TEST(TestPricePriority);
    RUN_TEST(TestTimePriority_FIFO);
    RUN_TEST(TestMarketOrderBuy);
    RUN_TEST(TestMarketOrderSell);
    RUN_TEST(TestMarketOrderEmptyBook);
    RUN_TEST(TestImmediateOrCancel_PartialFill);
    RUN_TEST(TestImmediateOrCancel_NoMatch);
    RUN_TEST(TestFillOrKill_FullFill);
    RUN_TEST(TestFillOrKill_PartialAvailable);
    RUN_TEST(TestFillOrKill_MultipleOrders);
    RUN_TEST(TestOrderModify);
    RUN_TEST(TestOrderbookLevelInfos);
    RUN_TEST(TestExchangeRulesBasic);
    RUN_TEST(TestMinNotionalValidation);
    RUN_TEST(TestMarketOrderValidation);
    RUN_TEST(TestFIFO_CancelMiddle_PreservesOrder);
    RUN_TEST(TestFIFO_CancelFirst_PreservesOrder);
    RUN_TEST(TestFIFO_MultipleCancels_PreservesOrder);
    RUN_TEST(TestTwoMarketOrders_NoInvalidTrade);
    RUN_TEST(TestModify_CannotChangeSide);
    RUN_TEST(TestModify_SameSide_Works);
    RUN_TEST(TestModify_QuantityOnly_PreservesTimePriority);

    std::cout << "\nAll 28 functionality tests passed!\n";

    PrintPerformanceHeader();

    std::cout << "--- Order Addition Performance ---\n";
    BenchmarkAddOrders(1000);
    BenchmarkAddOrders(10000);
    BenchmarkAddOrders(100000);

    std::cout << "--- Order Matching Performance ---\n";
    BenchmarkMatching(1000);
    BenchmarkMatching(10000);
    BenchmarkMatching(50000);

    std::cout << "--- Order Cancellation Performance ---\n";
    BenchmarkCancelOrders(1000);
    BenchmarkCancelOrders(10000);
    BenchmarkCancelOrders(100000);

    std::cout << "--- Order Modification Performance ---\n";
    BenchmarkModifyOrders(1000);
    BenchmarkModifyOrders(10000);

    std::cout << "--- Market Data Snapshot Performance ---\n";
    BenchmarkGetOrderInfos(1000, 1000);
    BenchmarkGetOrderInfos(10000, 1000);

    std::cout << "--- High-Frequency Trading Simulation ---\n";
    BenchmarkHighFrequencyTrading();

    std::cout << "\nTesting complete!\n";
    return 0;
}
