#pragma once

#include <memory>
#include <format>
#include <stdexcept>
#include "Types.h"
#include "Ordertype.h"
#include "Constants.h"

class Order {
    // represents one individual order
public:
    Order(OrderType orderType, OrderId orderId, Side side, Price price, Quantity quantity)
        : orderType_{orderType}
          , orderId_{orderId}
          , side_{side}
          , price_{price}
          , initialQuantity_{quantity}
          , remainingQuantity_{quantity} {
        if (quantity == 0) {
            throw std::invalid_argument("Order quantity must be greater than zero");
        }
    }

    Order(OrderId orderId, Side side, Quantity quantity)
        : Order(OrderType::Market, orderId, side, Constants::InvalidPrice, quantity) {
    }

    OrderId GetOrderId() const { return orderId_; }
    Side GetSide() const { return side_; }
    Price GetPrice() const { return price_; }
    OrderType GetOrderType() const { return orderType_; }
    Quantity GetInitialQuantity() const { return initialQuantity_; }
    Quantity GetRemainingQuantity() const { return remainingQuantity_; }
    Quantity GetFilledQuantity() const { return GetInitialQuantity() - GetRemainingQuantity(); }
    bool IsFilled() const { return GetRemainingQuantity() == 0; }

    void Fill(Quantity quantity) {
        // Fills part of an order,
        if (quantity > GetRemainingQuantity()) {
            throw std::logic_error(std::format("Order ({}) cannot be filled for more than its remaining quantity.",
                                               GetOrderId()));
        }
        remainingQuantity_ -= quantity;
    }

    void ToGoodTillCancel(Price price) {
        if (orderType_ != OrderType::Market) {
            throw std::logic_error("Cannot convert non-market order to GoodTillCancel");
        }

        price_ = price;
        orderType_ = OrderType::GoodTillCancel;
    }

    // Quantity-only modify: updates remaining quantity in place, preserving time priority.
    // Only valid when the price has not changed — caller is responsible for that check.
    void UpdateQuantity(Quantity newQuantity) {
        if (newQuantity == 0)
            throw std::invalid_argument("Order quantity must be greater than zero");
        initialQuantity_ = newQuantity;
        remainingQuantity_ = newQuantity;
    }

    // Lazy deletion: mark this order cancelled without touching the price-level container.
    // The entry is left in the deque and dropped from the front during the next matching pass.
    void Cancel() { cancelled_ = true; }
    bool IsCancelled() const { return cancelled_; }

private:
    OrderType orderType_;
    OrderId orderId_;
    Side side_;
    Price price_;
    Quantity initialQuantity_;
    Quantity remainingQuantity_;
    bool cancelled_ = false;
};