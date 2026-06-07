# High-Performance Order Book Engine

A low-latency limit order book implementation in C++20 with real-time market data integration. Built to handle
high-frequency trading workloads with nanosecond-level latency and zero heap allocations on the hot path.

## Overview

This project implements a matching engine and order book that supports multiple order types, priority-based matching,
and real-time market data processing. The architecture is explicitly designed for maximum cache locality and branch-prediction efficiency.

**Key metrics (CPU-pinned):**

- Order insertion: ~11,400,000 orders/sec (85 ns P50)
- Order matching: ~6,900,000 matches/sec, ~13,700,000 trades/sec
- Order cancellation: ~31,400,000 cancels/sec (32 ns P50)
- Market data snapshots: ~21,000,000 snapshots/sec

## Features

### Core Order Book

- **Order Types**: GoodTillCancel, Market, ImmediateOrCancel, FillOrKill, GoodForDay
- **Matching Algorithm**: Price-time priority (FIFO within price levels)
- **Zero-Allocation Architecture**: Pre-allocated flat object pool (`std::vector<PoolOrder>`) avoids OS allocator overhead.
- **Intrusive Doubly Linked List**: Guarantees strict O(1) order cancellation without vector/deque shifting.
- **SIMD Metadata Probing**: `absl::flat_hash_map` provides rapid O(1) Order ID lookup via open-addressing.

### Market Data Feed

- Real-time orderbook snapshots via Binance REST API
- Incremental update processing (new orders, cancellations, modifications)
- Latency monitoring and statistics

### Live Market Display

- Real-time visualization of cryptocurrency orderbooks
- Configurable refresh rates and depth levels
- Bid-ask spread analysis and mid-price calculation

## Build Instructions

### Requirements

- CMake 3.14+
- C++20 compatible compiler (GCC 10+, Clang 10+, MSVC 2019+)
- Dependencies (automatically fetched via CMake):
  - libcurl 8.4.0
  - nlohmann/json 3.11.3
  - abseil-cpp (20240116.2)

### Build

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --target OrderBookTests -j$(nproc)
cmake --build . --target LiveMarketData -j$(nproc)
```

### Run

```bash
# Run functionality and performance tests
./build/OrderBookTests

# Live cryptocurrency orderbook (e.g. SOLUSDT, 1 second refresh, 20 levels)
./build/LiveMarketData SOLUSDT 1 20
```

## Performance Characteristics

| Operation      | Complexity | Measured Throughput        | Measured Latency (P50) |
|----------------|------------|----------------------------|------------------------|
| Add Order      | O(log P)   | ~11,400,000 ops/sec        | ~87 ns                 |
| Cancel Order   | O(1)       | ~31,400,000 ops/sec        | ~32 ns                 |
| Modify Order   | O(log P)   | ~10,900,000 ops/sec        | ~91 ns                 |
| Match Orders   | O(M)       | ~6,900,000 matches/sec     | -                      |
| Snapshot       | O(P)       | ~21,000,000 ops/sec        | ~46 ns                 |

*P = number of active price levels, M = number of executed matches*
*Benchmarked via `tests.cpp` on Linux with CPU affinity pinning*

## Implementation Notes

### Design Decisions

**Why a pre-allocated flat pool instead of `shared_ptr`?**
Dynamic memory allocation (`new`/`delete`) forces context switching and breaks CPU cache locality. By pre-allocating a `std::vector<PoolOrder>` and managing a LIFO `freeList_` of integers, order creation and destruction are reduced to simple integer arithmetic, eliminating heap fragmentation.

**Why an Intrusive Doubly Linked List?**
Traditional deques require shifting elements or scanning when an order is cancelled from the middle of a price level. By embedding `prev` and `next` indices directly inside the `PoolOrder` struct, cancellations map from OrderID -> Index -> O(1) pointer stitch, entirely avoiding O(N) array scans.

**Why `absl::flat_hash_map` instead of `std::unordered_map`?**
`std::unordered_map` uses chained hashing (linked lists), causing pointer chasing on collisions. `absl::flat_hash_map` uses open addressing and a metadata array, allowing SSE2 instructions to check 16 bucket states simultaneously, drastically cutting cache misses.

### Order Type Behavior

- **GoodTillCancel**: Remains active until filled or cancelled
- **Market**: Immediately converted to aggressive limit order at sentinel prices (`INT_MAX`/`INT_MIN`)
- **ImmediateOrCancel**: Partial fills accepted, unfilled portion cancelled
- **FillOrKill**: All-or-nothing execution via two-phase (collect-then-execute) logic
- **GoodForDay**: Cancelled at configured time (default 15:59)

## Limitations

- Single instrument (no multi-asset support)
- No persistence layer
- Live data uses polling instead of WebSocket streaming
- No regulatory compliance features (audit logs, trade reporting)

## Future Enhancements

- TCP/UDP WebSocket integration for true tick-by-tick ingestion (bypassing JSON overhead via direct binary struct mapping).