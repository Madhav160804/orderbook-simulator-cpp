#pragma once

#include <cstdint>
#include <deque>
#include <memory>

class Order;

using Price = std::int32_t;
using Quantity = std::uint32_t;
using OrderId = std::uint64_t;