#pragma once

// ============================================================
// Orderbook — High-Performance Matching Engine
//
// Architecture: Global Object Pool + Intrusive Doubly Linked List
//               + absl::flat_hash_map for O(1) order lookup
//
// Key properties:
//   - Zero OS allocator calls during trading (pool pre-allocated at startup)
//   - Immediate O(1) cancellation via DLL pointer stitch — no dead entries
//   - absl::flat_hash_map: open-addressing + SIMD metadata scan,
//     2–4× faster than std::unordered_map for integer key lookups
//   - O(P) GetOrderInfos: totalQty maintained per price level
//   - Price-time (FIFO) priority enforced by DLL append + head-first match
// ============================================================

#include <map>
#include <vector>
#include <optional>
#include <algorithm>
#include <numeric>
#include <limits>
#include <stdexcept>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <sstream>
#include "absl/container/flat_hash_map.h"
#include "Order.h"
#include "OrderModify.h"
#include "Trade.h"
#include "LevelInfo.h"
#include "Types.h"
#include "Ordertype.h"
#include "Constants.h"
#include "MarketDataFeed.h"
#include "ExchangeRules.h"

class Orderbook {
public:
    static constexpr int32_t NULL_IDX  = -1;
    static constexpr size_t  POOL_SIZE = 1'000'000;

private:
    // ── Pool Order ────────────────────────────────────────────────────────
    // Stored flat in pool_[]. Embeds DLL links so no separate node
    // allocation is ever needed. Cancellation = 2 pointer writes + free_list push.
    struct PoolOrder {
        OrderId   orderId           = 0;
        Side      side              = Side::Buy;
        Price     price             = 0;
        OrderType orderType         = OrderType::GoodTillCancel;
        Quantity  quantity = 0;
        int32_t   prev              = NULL_IDX; // previous in same price level DLL
        int32_t   next              = NULL_IDX; // next in same price level DLL

        bool IsFilled()  const noexcept { return quantity == 0; }
        void Fill(Quantity q) noexcept  { quantity -= q; }
    };

    // ── Price Level ───────────────────────────────────────────────────────
    // head = oldest order (matched first).
    // tail = newest order (appended here on insert).
    // totalQty is kept in sync incrementally so GetOrderInfos is O(P).
    struct PriceLevel {
        int32_t  head       = NULL_IDX;
        int32_t  tail       = NULL_IDX;
        Quantity totalQty   = 0;
        int32_t  orderCount = 0;

        bool empty() const noexcept { return orderCount == 0; }
    };

    // ── Storage ───────────────────────────────────────────────────────────
    std::vector<PoolOrder> pool_;
    std::vector<int32_t>   freeList_;

    std::map<Price, PriceLevel, std::greater<Price>> bids_; // best bid first
    std::map<Price, PriceLevel, std::less<Price>>    asks_; // best ask first

    // absl::flat_hash_map: open-addressing, SIMD probe, zero per-entry allocs.
    // Every CancelOrder and every match executes an orders_.find(id) — this
    // is the single hottest lookup in the engine.
    absl::flat_hash_map<OrderId, int32_t> orders_; // id → pool index

    MarketDataStats stats_;
    uint64_t        lastSequenceNumber_ = 0;
    bool            isInitialized_      = false;
    ExchangeRules   exchangeRules_;

    // ── Pool alloc / free ─────────────────────────────────────────────────

    int32_t Alloc() {
        if (freeList_.empty())
            throw std::runtime_error("Orderbook: pool exhausted — increase POOL_SIZE");
        int32_t idx = freeList_.back();
        freeList_.pop_back();
        pool_[idx]       = PoolOrder{};
        return idx;
    }

    void Free(int32_t idx) noexcept {
        freeList_.push_back(idx);
    }

    // ── Intrusive DLL ─────────────────────────────────────────────────────

    // Append to tail — FIFO: head is oldest (matched first).
    void DllAppend(PriceLevel& lvl, int32_t idx) noexcept {
        pool_[idx].prev = lvl.tail;
        pool_[idx].next = NULL_IDX;
        if (lvl.tail != NULL_IDX) pool_[lvl.tail].next = idx;
        else                      lvl.head = idx;
        lvl.tail = idx;
        lvl.totalQty += pool_[idx].quantity;
        lvl.orderCount++;
    }

    // Remove from any position in O(1) — stitch prev.next and next.prev.
    // This is what makes immediate cancellation viable without a deque drain.
    void DllRemove(PriceLevel& lvl, int32_t idx) noexcept {
        auto& o = pool_[idx];
        if (o.prev != NULL_IDX) pool_[o.prev].next = o.next;
        else                    lvl.head = o.next;
        if (o.next != NULL_IDX) pool_[o.next].prev = o.prev;
        else                    lvl.tail = o.prev;
        lvl.totalQty -= o.quantity;
        lvl.orderCount--;
        o.prev = o.next = NULL_IDX;
    }

    // ── Helpers ───────────────────────────────────────────────────────────

    bool CanMatch(Side side, Price price) const noexcept {
        if (side == Side::Buy) {
            if (asks_.empty()) return false;
            return price >= asks_.begin()->first;
        } else {
            if (bids_.empty()) return false;
            return price <= bids_.begin()->first;
        }
    }

    bool ValidateOrder(OrderId id, Price price, Quantity qty) const {
        if (orders_.contains(id)) return false; // duplicate order ID

        bool isSentinel = (price == std::numeric_limits<Price>::max() ||
                           price == std::numeric_limits<Price>::min());

        if (!isSentinel && !exchangeRules_.IsValidPrice(price))         return false;
        if (!exchangeRules_.IsValidQuantity(qty))                       return false;
        if (!isSentinel && !exchangeRules_.IsValidNotional(price, qty)) return false;
        return true;
    }

    void InsertIntoBook(int32_t idx) {
        auto& o   = pool_[idx];
        PriceLevel& lvl = (o.side == Side::Buy) ? bids_[o.price] : asks_[o.price];
        DllAppend(lvl, idx);
        orders_[o.orderId] = idx;
    }

    // ── FillOrKill two-phase ──────────────────────────────────────────────
    // Phase 1: collect potential matches without touching state.
    // Phase 2: execute only if the full quantity can be filled.
    // This guarantees atomicity — no partial fills ever occur for FOK.

    struct FokMatch { int32_t idx; Quantity qty; };

    std::vector<FokMatch> CollectFOK(Side side, Price aggrPrice, Quantity remaining) const {
        std::vector<FokMatch> matches;
        auto scan = [&](const auto& levelMap) {
            for (const auto& [price, lvl] : levelMap) {
                if (side == Side::Buy  && price > aggrPrice) break;
                if (side == Side::Sell && price < aggrPrice) break;
                int32_t cur = lvl.head;
                while (cur != NULL_IDX && remaining > 0) {
                    Quantity q = std::min(remaining, pool_[cur].quantity);
                    matches.push_back({cur, q});
                    remaining -= q;
                    cur = pool_[cur].next;
                }
                if (remaining == 0) break;
            }
        };
        if (side == Side::Buy) scan(asks_);
        else                   scan(bids_);
        return matches;
    }

    Trades ExecuteFOK(int32_t aggrIdx, const std::vector<FokMatch>& matches) {
        Trades trades;
        auto& aggr = pool_[aggrIdx];

        for (auto& [matchIdx, qty] : matches) {
            auto& mo = pool_[matchIdx];
            Price tp = mo.price;

            // Update level totalQty before Fill (quantity still holds full value)
            PriceLevel& matchLvl = (mo.side == Side::Buy) ? bids_.at(mo.price)
                                                           : asks_.at(mo.price);
            matchLvl.totalQty -= qty;

            aggr.Fill(qty);
            mo.Fill(qty);

            if (aggr.side == Side::Buy) {
                trades.emplace_back(aggr.orderId, mo.orderId, tp, qty);
            } else {
                trades.emplace_back(mo.orderId, aggr.orderId, tp, qty);
            }

            if (mo.IsFilled()) {
                orders_.erase(mo.orderId);
                DllRemove(matchLvl, matchIdx);  // quantity==0, subtracts 0 from totalQty
                if (matchLvl.empty()) {
                    if (mo.side == Side::Buy) bids_.erase(mo.price);
                    else                      asks_.erase(mo.price);
                }
                Free(matchIdx);
            }
        }
        // FOK aggressor never enters the book — return its slot immediately
        Free(aggrIdx);
        return trades;
    }

    // ── Core matching loop ────────────────────────────────────────────────
    // Matches best bid against best ask until no cross exists.
    // aggressorSide determines trade price (resting side sets the price).

    Trades MatchOrders(Side aggressorSide, std::optional<OrderId> iocId = {}) {
        Trades trades;

        while (!bids_.empty() && !asks_.empty()) {
            auto bidIt = bids_.begin();
            auto askIt = asks_.begin();
            Price     bidPrice = bidIt->first;
            Price     askPrice = askIt->first;
            PriceLevel& bidLvl = bidIt->second;
            PriceLevel& askLvl = askIt->second;

            if (bidPrice < askPrice) break; // no cross

            // Two market orders that survived partial filling: a market buy
            // converts to GTC@INT_MAX and a market sell to GTC@INT_MIN.
            // INT_MAX >= INT_MIN so they appear to cross, but there is no
            // valid trade price between them — break to avoid an invalid match.
            bool bidIsMarket = (bidPrice == std::numeric_limits<Price>::max());
            bool askIsMarket = (askPrice == std::numeric_limits<Price>::min());
            if (bidIsMarket && askIsMarket) break;

            // Resting (passive) side sets the trade price.
            Price tradePrice;
            if      (bidIsMarket)                tradePrice = askPrice;
            else if (askIsMarket)                tradePrice = bidPrice;
            else if (aggressorSide == Side::Buy) tradePrice = askPrice;
            else                                 tradePrice = bidPrice;

            while (bidLvl.head != NULL_IDX && askLvl.head != NULL_IDX) {
                int32_t    bidIdx = bidLvl.head;
                int32_t    askIdx = askLvl.head;
                PoolOrder& bid    = pool_[bidIdx];
                PoolOrder& ask    = pool_[askIdx];

                Quantity qty = std::min(bid.quantity, ask.quantity);

                trades.emplace_back(bid.orderId, ask.orderId, tradePrice, qty);

                // Decrement level totals before Fill so we subtract correct qty
                bidLvl.totalQty -= qty;
                askLvl.totalQty -= qty;

                bid.Fill(qty);
                ask.Fill(qty);

                if (bid.IsFilled()) {
                    orders_.erase(bid.orderId);
                    DllRemove(bidLvl, bidIdx);
                    Free(bidIdx);
                }
                if (ask.IsFilled()) {
                    orders_.erase(ask.orderId);
                    DllRemove(askLvl, askIdx);
                    Free(askIdx);
                }
            }

            if (bidLvl.empty()) bids_.erase(bidIt);
            if (askLvl.empty()) asks_.erase(askIt);
        }

        // Cancel unfilled IOC remainder — immediate DLL removal, no scan needed
        if (iocId.has_value()) CancelOrder(iocId.value());
        return trades;
    }

    // ── Market data feed handlers ─────────────────────────────────────────

    void ProcessNewOrder(const NewOrderMessage& msg) {
        try {
            auto trades = AddOrder(msg.orderType, msg.orderId, msg.side, msg.price, msg.quantity);
            stats_.newOrders++;
            stats_.trades += trades.size();
        } catch (const std::exception&) {
            stats_.errors++;
        }
    }

    void ProcessCancel(const CancelOrderMessage& msg) {
        CancelOrder(msg.orderId);
        stats_.cancellations++;
    }

    void ProcessModify(const ModifyOrderMessage& msg) {
        OrderModify modify(msg.orderId, msg.side, msg.newPrice, msg.newQuantity);
        MatchOrder(modify);
        stats_.modifications++;
    }

    void ProcessTrade(const TradeMessage&) {
        stats_.trades++;
    }

    void ProcessSnapshot(const BookSnapshotMessage& msg) {
        // Clear existing book: cancel all live orders (proper DLL cleanup + pool free)
        std::vector<OrderId> toCancel;
        toCancel.reserve(orders_.size());
        for (const auto& [id, _] : orders_) toCancel.push_back(id);
        for (auto id : toCancel) CancelOrder(id);

        // Synthetic IDs start high to avoid collision with real order IDs
        OrderId syntheticId = 0x8000000000000000ULL;

        for (const auto& level : msg.bids) {
            if (level.quantity == 0) continue;
            try {
                AddOrder(OrderType::GoodTillCancel, syntheticId++,
                         Side::Buy, level.price, level.quantity);
            } catch (...) { continue; }
        }
        for (const auto& level : msg.asks) {
            if (level.quantity == 0) continue;
            try {
                AddOrder(OrderType::GoodTillCancel, syntheticId++,
                         Side::Sell, level.price, level.quantity);
            } catch (...) { continue; }
        }

        isInitialized_      = true;
        lastSequenceNumber_ = msg.sequenceNumber;
        stats_.snapshots++;
    }

public:
    // ── Construction ──────────────────────────────────────────────────────

    explicit Orderbook(size_t poolSize = POOL_SIZE) {
        pool_.resize(poolSize);
        freeList_.reserve(poolSize);
        orders_.reserve(poolSize / 4); // absl pre-size to reduce rehashes
        for (int32_t i = static_cast<int32_t>(poolSize) - 1; i >= 0; --i)
            freeList_.push_back(i); // index 0 allocated first (LIFO free list)
    }

    // ── Exchange rules ────────────────────────────────────────────────────

    void SetExchangeRules(const ExchangeRules& rules) { exchangeRules_ = rules; }
    const ExchangeRules& GetExchangeRules() const     { return exchangeRules_; }

    // ── Core operations ───────────────────────────────────────────────────

    Trades AddOrder(OrderType type, OrderId id, Side side, Price price, Quantity qty) {
        // Market order: convert to limit at INT_MAX (buy) / INT_MIN (sell)
        // so it matches any resting order. Reject if the opposing side is empty.
        if (type == OrderType::Market) {
            if (side == Side::Buy && !asks_.empty()) {
                price = std::numeric_limits<Price>::max();
                type  = OrderType::GoodTillCancel;
            } else if (side == Side::Sell && !bids_.empty()) {
                price = std::numeric_limits<Price>::min();
                type  = OrderType::GoodTillCancel;
            } else {
                return {}; // no liquidity on opposing side
            }
        }

        if (!ValidateOrder(id, price, qty)) return {};

        // IOC: if there is nothing to match at this price, discard immediately
        if (type == OrderType::ImmediateOrCancel && !CanMatch(side, price)) return {};

        // FOK: two-phase — collect → check → execute atomically (never enters book)
        if (type == OrderType::FillOrKill) {
            int32_t idx = Alloc();
            pool_[idx].orderId           = id;
            pool_[idx].side              = side;
            pool_[idx].price             = price;
            pool_[idx].orderType         = type;
            pool_[idx].quantity = qty;
            auto matches = CollectFOK(side, price, qty);
            Quantity total = 0;
            for (auto& m : matches) total += m.qty;
            if (total < qty) { Free(idx); return {}; } // can't fill fully — kill
            return ExecuteFOK(idx, matches);
        }

        // All other types: insert into book and run matching
        int32_t idx = Alloc();
        pool_[idx].orderId           = id;
        pool_[idx].side              = side;
        pool_[idx].price             = price;
        pool_[idx].orderType         = type;
        pool_[idx].quantity = qty;
        InsertIntoBook(idx);

        const bool isIoc = (type == OrderType::ImmediateOrCancel);
        return MatchOrders(side, isIoc ? std::optional<OrderId>{id}
                                       : std::optional<OrderId>{});
    }

    // O(1): hash map erase + DLL stitch + pool free.
    // No deque drain, no dead entries, no deferred cleanup.
    void CancelOrder(OrderId id) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return;
        int32_t    idx = it->second;
        PoolOrder& o   = pool_[idx];

        PriceLevel& lvl = (o.side == Side::Buy) ? bids_.at(o.price)
                                                 : asks_.at(o.price);
        DllRemove(lvl, idx);
        if (lvl.empty()) {
            if (o.side == Side::Buy) bids_.erase(o.price);
            else                     asks_.erase(o.price);
        }
        Free(idx);
        orders_.erase(it);
    }

    // Quantity-only modify: update in place, preserve time priority (DLL position).
    // Price change: cancel + re-add (order goes to back of new price level — correct
    // exchange behaviour, matches NASDAQ/CME rules).
    Trades MatchOrder(OrderModify mod) {
        auto it = orders_.find(mod.GetOrderId());
        if (it == orders_.end()) return {};
        PoolOrder& existing = pool_[it->second];
        if (mod.GetSide() != existing.side) return {}; // side-flip forbidden

        if (mod.GetPrice() == existing.price) {
            // Quantity-only: update totalQty cache, then update order in place
            PriceLevel& lvl = (existing.side == Side::Buy) ? bids_.at(existing.price)
                                                           : asks_.at(existing.price);
            lvl.totalQty -= existing.quantity;
            existing.quantity = mod.GetQuantity();
            lvl.totalQty += existing.quantity;
            return {};
        }

        // Price changed: cancel + re-add (loses time priority)
        OrderType t = existing.orderType;
        Side      s = existing.side;
        CancelOrder(mod.GetOrderId());
        return AddOrder(t, mod.GetOrderId(), s, mod.GetPrice(), mod.GetQuantity());
    }

    // Called by an external timer thread at end of trading day.
    // The book has no clock — the caller decides when.
    void CancelGoodForDayOrders() {
        std::vector<OrderId> toCancel;
        for (const auto& [id, idx] : orders_)
            if (pool_[idx].orderType == OrderType::GoodForDay)
                toCancel.push_back(id);
        for (auto id : toCancel) CancelOrder(id);
    }

    // ── Queries ───────────────────────────────────────────────────────────

    std::size_t Size() const noexcept { return orders_.size(); }

    bool HasOrder(OrderId id) const { return orders_.contains(id); }

    // O(P) — totalQty is maintained per level, no per-order scan needed.
    OrderbookLevelInfos GetOrderInfos() const {
        LevelInfos bidInfos, askInfos;
        bidInfos.reserve(bids_.size());
        askInfos.reserve(asks_.size());
        for (const auto& [price, lvl] : bids_)
            if (lvl.totalQty > 0) bidInfos.push_back({price, lvl.totalQty});
        for (const auto& [price, lvl] : asks_)
            if (lvl.totalQty > 0) askInfos.push_back({price, lvl.totalQty});
        return {bidInfos, askInfos};
    }

    // ── Market data feed ──────────────────────────────────────────────────

    bool ProcessMarketData(const MarketDataMessage& message) {
        auto startTime = std::chrono::high_resolution_clock::now();
        try {
            std::visit([this](auto&& msg) {
                using T = std::decay_t<decltype(msg)>;
                if constexpr (std::is_same_v<T, NewOrderMessage>)     ProcessNewOrder(msg);
                else if constexpr (std::is_same_v<T, CancelOrderMessage>) ProcessCancel(msg);
                else if constexpr (std::is_same_v<T, ModifyOrderMessage>) ProcessModify(msg);
                else if constexpr (std::is_same_v<T, TradeMessage>)       ProcessTrade(msg);
                else if constexpr (std::is_same_v<T, BookSnapshotMessage>) ProcessSnapshot(msg);
            }, message);

            auto endTime = std::chrono::high_resolution_clock::now();
            auto latency = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
            stats_.messagesProcessed++;
            stats_.totalProcessingTime += latency;
            stats_.maxLatency = std::max(stats_.maxLatency, latency);
            stats_.minLatency = std::min(stats_.minLatency, latency);
            return true;
        } catch (...) {
            stats_.errors++;
            return false;
        }
    }

    size_t ProcessMarketDataBatch(const std::vector<MarketDataMessage>& messages) {
        size_t ok = 0;
        for (const auto& msg : messages)
            if (ProcessMarketData(msg)) ok++;
        return ok;
    }

    const MarketDataStats& GetMarketDataStats() const { return stats_; }
    void ResetMarketDataStats()  { stats_.Reset(); }
    bool IsInitialized()   const { return isInitialized_; }
    uint64_t GetLastSequenceNumber() const { return lastSequenceNumber_; }

    // ── Pool diagnostics ──────────────────────────────────────────────────

    size_t PoolCapacity()  const noexcept { return pool_.size(); }
    size_t PoolFreeSlots() const noexcept { return freeList_.size(); }
    size_t PoolUsed()      const noexcept { return pool_.size() - freeList_.size(); }
};