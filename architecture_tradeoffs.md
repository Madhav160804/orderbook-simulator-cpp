# High-Frequency Orderbook Architecture Tradeoffs

When building a high-performance matching engine, the choice of data structure and cancellation strategy dictates not only your average throughput but, more importantly, your **P99 latency (jitter)**.

In algorithmic trading, consistency is often more valuable than raw average speed. A system that averages 100ns but occasionally spikes to 1ms (P99) is often considered worse than a system that consistently runs at 500ns.

---

## Cancellation Strategies

The most frequent operation in any modern orderbook is cancellation (often representing >95% of all message traffic). 

### 1. Lazy Cancellation
Instead of removing the order from the underlying memory structure (like a `vector` or `deque`), you simply flag the order as `is_cancelled = true` and remove it from your fast-lookup hash map.
* **Pros**: Always `O(1)` for the cancel itself. Zero memory shuffling. **Does NOT cause latency spikes** — each dead order is drained exactly once from the front of the deque during matching (amortized `O(1)`).
* **Cons**: Leaves "dead" memory trailing in your price levels. In extreme edge cases (e.g., 50,000 stale orders at a suddenly-active price level), the matching loop may spend a few microseconds draining them.
* **The flash-crash advantage**: During mass cancellation events (circuit breakers, market crashes), lazy cancellation shines. You are just flipping boolean flags, never calling `delete`. Immediate cancellation via DLL would be hammering the OS memory allocator with thousands of `delete` calls precisely when stability matters most.

### 2. Immediate Cancellation
The order is physically removed from the price level queue the moment the cancel request is received.
* **Pros**: Price levels are perfectly clean. The matching engine never wastes cycles looking at dead orders.
* **Cons**: Depending on the data structure, this can be an `O(N)` operation (shifting elements in a vector), or it forces you to use node-based structures like a Doubly Linked List, which requires an expensive `delete` (deallocation) call that thrashes the OS memory allocator.
* **The exception**: The Global Object Pool (Approach #6) achieves immediate cancellation *without* calling `delete` — the freed slot returns to the pool's own free list, bypassing the OS allocator entirely. This is the only form of immediate cancellation that is safe for HFT.

### In Practice: Who Uses What?

| Tier | Users | Strategy |
| :--- | :--- | :--- |
| **Exchange Engines** (Nasdaq, CME, Binance) | Exchange matching core | Global Pool + immediate cancellation (no `delete`) |
| **Market Making Firms** (Citadel, Virtu, Jane Street) | Internal book, co-located | Global Pool or `deque<Order>` + lazy cancellation |
| **Brokers / Venue Simulators** | Non-co-located systems | `deque<Order>` + lazy cancellation |
| **Backtesters / Research** | Academic, quant research | `shared_ptr` or `deque` + lazy cancellation |

> **Key takeaway**: Lazy cancellation is **extremely common in production** at all tiers except the very top. It is only replaced when you also replace the memory allocator — simply switching from lazy to DLL immediate cancellation *without* a pool makes things worse, not better.

---

## Data Structure Approaches

Below is a detailed comparison of the primary ways to build the price levels (`bids_` / `asks_`) in C++.

### 1. Doubly Linked List (`std::map<Price, std::list<Order>>`)
*The standard textbook approach to immediate cancellation.*

**Lookup Map**:
```cpp
struct OrderLocation {
    std::list<Order>* list_ptr;       // Pointer to the list at this price level (skips map traversal)
    std::list<Order>::iterator iter;  // Iterator to the exact node
};
std::unordered_map<OrderId, OrderLocation> lookup_; // 16 bytes per entry
```
> **Why both?** In C++, you cannot call `erase` on an iterator alone — you need the `std::list` object itself. Storing `list_ptr` lets you bypass the `O(log P)` map traversal entirely, keeping cancel at `O(1)`.

| Metric | Rating | Notes |
| :--- | :--- | :--- |
| **Insert Latency** | Poor | `O(log P)` map lookup to find the price level, plus a `new` heap allocation per list node. |
| **Cancel Latency** | Excellent | `O(1)` immediate. `lookup_[id].list_ptr->erase(lookup_[id].iter)` stitches the DLL in place. |
| **Lookup Entry Size** | Poor | 16 bytes per order (8-byte pointer + 8-byte iterator). |
| **P50 (Average)** | Moderate | Slower due to scattered heap memory and `new`/`delete` calls. |
| **P99 (Jitter)** | Poor | The OS memory allocator will occasionally cause massive latency spikes during high load. |
| **Cache Locality** | Terrible | Every node is randomly placed on the heap. Traversing a price level causes a cache miss on almost every order. |

### 2. The Baseline (`std::map<Price, std::deque<shared_ptr<Order>>>`)
*Your current implementation.*

**Lookup Map**:
```cpp
std::unordered_map<OrderId, std::shared_ptr<Order>> lookup_; // 16 bytes per entry (shared_ptr)
```
> The `shared_ptr` itself is 16 bytes (data pointer + control block pointer). Dereferencing it to reach the `Order` requires two pointer hops, causing double-indirection cache misses.

| Metric | Rating | Notes |
| :--- | :--- | :--- |
| **Insert Latency** | Good | `O(log P)` map lookup + `O(1)` deque push_back. Chunk-based allocation avoids per-order `new`. |
| **Cancel Latency** | Good | `O(1)` using lazy cancellation. Marks `is_cancelled`, erases from map. |
| **Lookup Entry Size** | Poor | 16 bytes per order (shared_ptr = data ptr + control block ptr). |
| **P50 (Average)** | Good | ~150-300ns. |
| **P99 (Jitter)** | Moderate | `shared_ptr` control block allocations still cause some allocator jitter. |
| **Cache Locality** | Poor | The deque chunks are contiguous, but the actual `Order` objects are on the heap (double indirection). |

### 3. The Pragmatic Value-Store (`std::map<Price, std::deque<Order>>`)
*Storing orders by value, using raw `Order*` in the hash map.*

**Lookup Map**:
```cpp
std::unordered_map<OrderId, Order*> lookup_; // 8 bytes per entry (raw pointer)
```
> `Order*` is a raw pointer that was taken via `&deque.back()` immediately after `push_back`. Per the C++ standard, `deque` **guarantees** that pointers and references to existing elements remain valid after `push_back` or `pop_front` (only iterators are invalidated). This makes raw pointer storage safe as long as you never insert/erase in the middle of the deque.

**Alternative — Index Math (no pointers)**:
```cpp
struct OrderLocation { Price price; uint64_t absolute_index; };
std::unordered_map<OrderId, OrderLocation> lookup_; // ~12 bytes per entry
// Deque index = absolute_index - level.popped_count
```
> Trades a tiny nanosecond-level calculation for complete pointer-safety and easy serialization.

| Metric | Rating | Notes |
| :--- | :--- | :--- |
| **Insert Latency** | Excellent | `O(log P)` map lookup + `O(1)` deque push_back. Zero per-order heap allocations. |
| **Cancel Latency** | Excellent | `O(1)` lazy. `Order*` dereference is a single `MOV` instruction (1 memory load). Index math requires 3–4 arithmetic ops + 2 memory loads (`deque` map pointer → chunk pointer → element) — ~2–5ns slower. The bottleneck in both cases is the cache miss to load the `Order` data itself, not the address calculation. |
| **Lookup Entry Size** | Excellent | 8 bytes per order (`Order*`), vs 16 bytes for `shared_ptr`. |
| **P50 (Average)** | Excellent | Expected ~50-100ns. |
| **P99 (Jitter)** | Good | Very few memory allocations. Occasional chunk allocation causes minor spikes. |
| **Cache Locality** | Great | Orders are tightly packed within the deque's internal 512B/4KB chunks. |

### 4. Naive Vector (`std::map<Price, std::vector<Order>>`)
*An attempt at perfect cache locality that falls into a memory trap.*

A `std::vector` can technically be used to hold the orders at each price level. You have three options for cancellation, each with a fundamental problem:

| Cancellation Method | Complexity | Problem |
| :--- | :--- | :--- |
| **Shift elements** (`erase` at index) | `O(N)` | Shifts every order after the cancelled one. Destroys latency at busy price levels. |
| **Lazy (mark cancelled, skip during match)** | `O(1)` cancel | Dead orders accumulate forever — unlike `deque`, there is no efficient `pop_front()` to clean them up. The vector grows unboundedly until the entire price level is wiped out. |
| **Swap-and-pop** (swap with last element, pop_back) | `O(1)` | Constant time, but **destroys FIFO time priority**. The swapped-in order now occupies an earlier position than it should. This violates a core exchange rule — earlier orders must fill first. |

> **The fundamental problem**: `std::vector` is a random-access, back-insertion container. An orderbook price level is a FIFO queue. These two things are inherently mismatched. Any attempt to do cheap cancellation on a `vector` either breaks FIFO, leaks memory, or is `O(N)`.

| Metric | Rating | Notes |
| :--- | :--- | :--- |
| **Insert Latency** | Good | `O(log P)` map lookup + amortized `O(1)` push_back. Reallocation triggers an `O(N)` copy of the entire price level. |
| **Cancel Latency** | Poor / Trap | Every cancellation option is broken: `O(N)` shift, memory-bloating lazy, or FIFO-violating swap-and-pop. |
| **P50 (Average)** | Great | Iterating for matches is blazing fast due to perfect cache locality. |
| **P99 (Jitter)** | Terrible | Vector reallocations cause massive, unpredictable latency spikes. |
| **Cache Locality** | Perfect | 100% contiguous memory — the one genuine advantage. |

### 5. Flat Price Array (`std::vector<std::deque<Order>>`)
*The traditional equities architecture, bypassing `std::map` entirely.*

| Metric | Rating | Notes |
| :--- | :--- | :--- |
| **Insert + Price Lookup** | Perfect | `O(1)` array index instead of `O(log P)` map traversal. This is the entire point of the flat array. |
| **P50 & P99** | Excellent | Removes tree traversal overhead entirely. |
| **Memory Bloat** | Terrible (Crypto) | For wide/sparse price ranges (like Bitcoin), you must allocate millions of empty deques, wasting gigabytes of RAM. |
| **Best Bid/Ask Scan**| Poor | If the best price empties, you must `O(N)` scan the vector to find the next active price (unless using complex bitmasks). |

### 6. The HFT Holy Grail (Global Object Pool + Intrusive Doubly Linked List)
*Zero dynamic allocations, using pre-allocated arrays and integer indices.*

**Lookup Map**:
```cpp
// The Order struct itself embeds the DLL links
struct Order {
    Price price; Quantity qty; /* ... */
    int32_t prev_index = -1;
    int32_t next_index = -1;
};
std::vector<Order> global_pool(10'000'000);  // Pre-allocated at startup
std::vector<int32_t> free_list;              // Recycled indices

std::unordered_map<OrderId, int32_t> lookup_; // 4 bytes per entry (int32_t index)
```
> The `std::map<Price, int32_t>` stores only the index of the **head** of the intrusive list for each price level. Cancellation stitches `pool[prev].next = next` and `pool[next].prev = prev` directly — no `new`, no `delete`, no STL allocator involved.

| Metric | Rating | Notes |
| :--- | :--- | :--- |
| **Insert Latency** | Perfect | `O(log P)` map lookup + `O(1)` pool index pop. Zero OS allocations during trading. |
| **Cancel Latency** | Perfect | `O(1)` immediate. DLL stitch using `int32_t` indices, no allocator involved. |
| **Lookup Entry Size** | Perfect | 4 bytes per order (`int32_t` index, half the size of a raw pointer). |
| **P50 (Average)** | Perfect | Expected <20ns. |
| **P99 (Jitter)** | Perfect | No OS memory allocator involved. Latency is completely deterministic and flat. |
| **Cache Locality** | Great | All orders live in one massive contiguous array. Using 32-bit indices instead of 64-bit pointers fits twice as many orders in L1 cache. |

---

## Summary Recommendation

### Industry Adoption Ladder

```
Beginner:      shared_ptr + lazy cancellation         ← your current repo (Approach #2)
               Simple, correct, easy to reason about.

Intermediate:  deque<Order> + Order* + lazy            ← most production non-HFT systems (Approach #3)
               Excellent latency, no allocator jitter, safe with std::deque pointer rules.

Advanced:      Global Pool + immediate cancellation    ← co-located HFT, exchange engines (Approach #6)
               Only necessary when competing on nanoseconds next to the exchange.
```

1. **If you want standard C++ and clean code**: Use **Approach #3** (`deque<Order>` + `Order*`). It solves the cache locality issue of your current implementation, removes `shared_ptr` bloat, and provides excellent average and P99 latency with no allocator spikes.
2. **If you are building for ultra-low latency (HFT)**: You must build **Approach #6**. The only way to guarantee a deterministically flat P99 is to completely remove `new`, `delete`, and STL allocators from the critical path using a pre-allocated memory arena.
3. **Never do this**: Use a `std::list` (DLL) with `new`/`delete` for immediate cancellation without a pool. You get the worst of both worlds — allocator jitter on every cancel AND terrible cache locality during matching.

---

## Other Hot Path Allocations to Avoid

Fixing the order book data structure is only half the battle. There are other hidden dynamic allocations lurking in the critical path that must also be eliminated.

### The `AddOrder` Return Value Problem

In the current implementation, `AddOrder` returns a `std::vector<Trade>` by value:

```cpp
// ❌ Current: constructs a new vector<Trade> on the heap for every call
Trades AddOrder(OrderPointer order);
```

Every single call to `AddOrder` that results in a trade must:
1. `new` — Heap-allocate the `std::vector<Trade>` return value.
2. Copy/move the trades into it.
3. `delete` — The caller destroys the vector when it goes out of scope.

This is a dynamic allocation on **every matched order**, right in the hottest path of the engine.

**The Fix: Output Parameter Pattern**

Pass a pre-allocated `std::vector<Trade>` by reference as an output argument instead. The caller owns the buffer, clears it before each call, and the engine just `push_back`s into it:

```cpp
// ✅ HFT style: zero allocation — reuses the caller's pre-allocated buffer
void AddOrder(OrderPointer order, std::vector<Trade>& outTrades);

// Caller side:
std::vector<Trade> trades;
trades.reserve(1024); // pre-allocate once at startup

while (true) {
    trades.clear();             // O(1) — just resets size, no deallocation
    book.AddOrder(order, trades);
    process(trades);
}
```

`std::vector::clear()` sets the size to zero but **does not free the underlying memory**. So after the first few calls warm up the buffer's capacity, every subsequent call has zero heap allocations, even when hundreds of trades are generated by a single aggressive order sweeping through the book.

| Approach | Allocation per `AddOrder` call | Notes |
| :--- | :--- | :--- |
| **Return by value** (current) | 1× `new` + 1× `delete` always | Allocator hit on every matched order |
| **Output parameter** (fix) | Zero (after warmup) | Buffer reused across calls indefinitely |

> This pattern generalises to any function that currently returns a `std::vector` in the hot path: `ModifyOrder`, `GetOrderInfos`, `ProcessMarketDataBatch`, etc. All of them should accept pre-allocated output buffers in a truly latency-sensitive system.

---

## Additional Optimizations

### 1. Order Struct Layout & Cache Line Alignment
The current `Order` struct has a problematic layout due to C++ padding rules:

```cpp
// Current layout (implicit padding wastes bytes):
class Order {
    OrderType orderType_;       // 4 bytes (enum)
    OrderId   orderId_;         // 8 bytes → compiler inserts 4 bytes padding here!
    Side      side_;            // 4 bytes (enum)
    Price     price_;           // 4 bytes
    Quantity  initialQuantity_; // 4 bytes
    Quantity  quantity_; // 4 bytes
    bool      cancelled_;       // 1 byte → 3 bytes padding after
    // Total: ~36 bytes with padding, not 33
};
```

Reorder fields largest-to-smallest to eliminate padding:
```cpp
// Optimised layout — no wasted bytes:
class Order {
    OrderId   orderId_;           // 8 bytes
    Price     price_;             // 4 bytes
    Quantity  initialQuantity_;   // 4 bytes
    Quantity  quantity_; // 4 bytes
    OrderType orderType_;         // 4 bytes (enum, assume int)
    Side      side_;              // 1 byte  (can make uint8_t)
    bool      cancelled_;         // 1 byte
    // Total: 26 bytes → fits ~2.4 orders per 64-byte cache line instead of ~1.7
};
```
More orders per cache line = fewer cache misses during matching.

---

### 2. `std::map` Cache Locality Problem
`std::map` uses a **red-black tree** internally. Each tree node is heap-allocated separately, so traversing from root to a price level touches 3–5 scattered memory addresses (one per tree level), causing a cache miss at each step.

For ultra-low latency, teams replace `std::map` with:
- **`absl::btree_map`** (Google Abseil): Stores multiple keys per node, dramatically improving cache locality for tree traversal.
- **Custom skip list**: Probabilistic structure with better cache behaviour for sequential access.
- **Flat array** (for equities): Eliminates the tree entirely.

In practice, for small books (< 500 price levels), the top of the `std::map` tree often stays hot in L2 cache anyway, so this is a micro-optimisation that only matters for very deep books.

---

### 3. `std::unordered_map` Hash Collision Risk
The default `std::unordered_map` uses chaining for collisions (linked-list of entries per bucket). In the worst case this degrades to `O(N)` lookup. For `OrderId` lookups on the hot path this is dangerous.

Better alternatives:
- **`absl::flat_hash_map`**: Open-addressing with SIMD probing. Often 2–4× faster than `std::unordered_map` for integer keys.
- **Robin Hood hashing** (`ankerl::unordered_dense`): Cache-friendly open-addressing with low variance.
- **Custom power-of-2 table**: Replace `% bucket_count` with `& (size - 1)` (bitwise AND), turning the modulo into a single CPU instruction.

---

### 4. `std::chrono` in the Hot Path
In your `ProcessMarketData`, every single message pays a syscall to read the hardware timer:

```cpp
// ❌ Two syscalls per message — can take 20-50ns each on Linux
auto startTime = std::chrono::high_resolution_clock::now();
// ... process message ...
auto endTime = std::chrono::high_resolution_clock::now();
```

**Fixes:**
- Use `RDTSC` (CPU cycle counter) instead — it's a single instruction with ~1ns overhead vs ~20-50ns for a `clock_gettime` syscall.
- Only sample latency on **every Nth message** (e.g., every 1000) using a counter, and compute statistics from the sampled subset.
- Move latency tracking **entirely off the hot path** into a separate monitoring thread.

---

### 5. `std::optional` Overhead in `MatchOrders`
The current signature passes an optional IOC order ID:

```cpp
// std::optional adds a branch + a boolean byte on every call
Trades MatchOrders(Side aggressorSide, std::optional<OrderId> iocOrderId = {});
```

In the hot path, `std::optional` introduces a branch to check `.has_value()` on every call. The fix is to use a **sentinel value** instead:

```cpp
// Zero branches — sentinel value signals "no IOC order"
static constexpr OrderId NO_IOC = std::numeric_limits<OrderId>::max();
Trades MatchOrders(Side aggressorSide, OrderId iocOrderId = NO_IOC);
```

---

### 6. `noexcept` Annotations
Many hot-path functions like `Fill`, `Cancel`, `IsFilled`, and `MatchOrders` can throw exceptions in error cases. Marking them `noexcept` where safe allows the compiler to:
- Skip generating exception-handling unwinding tables.
- Apply more aggressive inlining and optimisation.
- Enable `std::vector` to use `std::move` instead of `std::copy` during reallocation (critical for `vector<Order>`).

```cpp
void Cancel() noexcept { cancelled_ = true; }
bool IsCancelled() const noexcept { return cancelled_; }
bool IsFilled() const noexcept { return quantity_ == 0; }
```

---

### 7. Drop-in Allocator Replacement (Easy Win)
Before doing any of the above, the single easiest performance improvement is replacing the default OS allocator (`glibc malloc`) with a high-performance alternative. This requires **zero code changes** — just link against the library:

```bash
# jemalloc: reduces fragmentation, better multi-threaded performance
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so ./OrderBookTests

# tcmalloc (Google): thread-cached allocator, excellent for short-lived objects
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtcmalloc.so ./OrderBookTests
```

Expected improvement: **15–40% reduction in P99 latency** for allocation-heavy workloads, with zero code changes.

---

### 8. CPU Affinity & Thread Pinning
Even a perfectly optimised matching engine will have P99 spikes if the OS scheduler migrates it to a different CPU core mid-execution. Modern CPUs have separate L1/L2 caches per core — a core migration instantly cold-caches all your hot data.

```cpp
// Pin the matching engine thread to core 3
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(3, &cpuset);
pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
```

In production co-location, the matching engine core is also **isolated** (`isolcpus=3` in kernel boot parameters) so the OS scheduler never steals it for other processes.
