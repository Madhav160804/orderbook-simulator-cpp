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
          , quantity_{quantity} {
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
    Quantity GetQuantity() const { return quantity_; }
    bool IsFilled() const { return GetQuantity() == 0; }

    void Fill(Quantity quantity) {
        // Fills part of an order,
        if (quantity > GetQuantity()) {
            throw std::logic_error(std::format("Order ({}) cannot be filled for more than its remaining quantity.",
                                               GetOrderId()));
        }
        quantity_ -= quantity;
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
        quantity_ = newQuantity;
    }

private:
    OrderType orderType_;
    OrderId orderId_;
    Side side_;
    Price price_;
    Quantity quantity_;
};