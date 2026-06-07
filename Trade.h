#pragma once

#include <vector>
#include "Types.h"

class Trade {
public:
    Trade(OrderId bidOrderId, OrderId askOrderId, Price price, Quantity quantity)
        : bidOrderId_{bidOrderId}
        , askOrderId_{askOrderId}
        , price_{price}
        , quantity_{quantity} {
    }

    OrderId GetBidOrderId() const { return bidOrderId_; }
    OrderId GetAskOrderId() const { return askOrderId_; }
    Price GetPrice() const { return price_; }
    Quantity GetQuantity() const { return quantity_; }

private:
    OrderId bidOrderId_;
    OrderId askOrderId_;
    Price price_;
    Quantity quantity_;
};

using Trades = std::vector<Trade>;