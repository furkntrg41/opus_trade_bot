// ============================================================================
// OPUS TRADE BOT - Binance Futures Client Implementation
// ============================================================================
// High-level Binance API client with REST and WebSocket support
// ============================================================================

#include "opus/exchange/binance/client.hpp"

#include <simdjson.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

#include <iomanip>
#include <cmath>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace opus::exchange::binance {

// ============================================================================
// JSON Parsing Helpers
// ============================================================================

static Price parse_price(std::string_view str) {
    return Price::from_double(std::stod(std::string(str)));
}

static Quantity parse_quantity(std::string_view str) {
    return Quantity::from_double(std::stod(std::string(str)));
}

static Side parse_side(std::string_view str) {
    return str == "BUY" ? Side::Buy : Side::Sell;
}

static OrderStatus parse_order_status(std::string_view str) {
    if (str == "NEW") return OrderStatus::New;
    if (str == "PARTIALLY_FILLED") return OrderStatus::PartiallyFilled;
    if (str == "FILLED") return OrderStatus::Filled;
    if (str == "CANCELED") return OrderStatus::Canceled;
    if (str == "REJECTED") return OrderStatus::Rejected;
    if (str == "EXPIRED") return OrderStatus::Expired;
    return OrderStatus::New;
}

static std::string side_to_string(Side side) {
    return side == Side::Buy ? "BUY" : "SELL";
}

static std::string order_type_to_string(OrderType type) {
    switch (type) {
        case OrderType::Limit: return "LIMIT";
        case OrderType::Market: return "MARKET";
        case OrderType::StopMarket: return "STOP_MARKET";
        case OrderType::StopLimit: return "STOP";
        case OrderType::TakeProfit: return "TAKE_PROFIT";
        case OrderType::TakeProfitMarket: return "TAKE_PROFIT_MARKET";
        default: return "MARKET";
    }
}

static std::string tif_to_string(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        case TimeInForce::GTX: return "GTX";
        default: return "GTC";
    }
}

// ============================================================================
// Binance Client Implementation
// ============================================================================

struct BinanceClient::Impl {
    BinanceConfig config_;
    std::unique_ptr<network::RestClient> rest_client_;
    std::unique_ptr<network::WebSocketClient> ws_client_;

    // Callbacks
    std::unordered_map<std::string, DepthCallback> depth_callbacks_;
    std::unordered_map<std::string, TradeCallback> trade_callbacks_;
    std::unordered_map<std::string, KlineCallback> kline_callbacks_;

    // JSON parser
    simdjson::ondemand::parser parser_;

    // State
    std::atomic<bool> connected_{false};
    std::mutex callback_mutex_;
    std::string last_error_;

    // Track active subscriptions for re-subscribe on reconnect
    std::vector<std::string> active_subscriptions_;

    explicit Impl(const BinanceConfig& config) : config_(config) {
        // Initialize REST client
        network::RestClientConfig rest_config;
        rest_config.api_key = config.api_key;
        rest_config.secret_key = config.secret_key;
        rest_config.use_testnet = config.testnet;
        rest_config.base_url = config.testnet ? config.testnet_rest_url : config.mainnet_rest_url;

        rest_client_ = std::make_unique<network::RestClient>(rest_config);

        // Initialize WebSocket client - use mainnet stream for public market data
        // Trading APIs use testnet, but market data stream is public
        network::WebSocketConfig ws_config;
        ws_config.host = "fstream.binance.com";  // Public market data stream
        ws_config.port = "443";
        ws_config.path = "/ws";  // Will subscribe via JSON message
        ws_config.ping_interval = std::chrono::minutes(3);
        ws_config.connect_timeout = std::chrono::seconds(20);

        ws_client_ = std::make_unique<network::WebSocketClient>(ws_config);
    }

    // ========================================================================
    // REST API Methods
    // ========================================================================

    std::optional<AccountInfo> get_account_info() {
        network::HttpRequest req;
        req.method = network::HttpMethod::GET;
        req.path = "/fapi/v2/account";
        req.sign = true;

        auto response = rest_client_->request(req);
        if (!response.is_success()) {
            return std::nullopt;
        }

        try {
            simdjson::padded_string padded(response.body);
            auto doc = parser_.iterate(padded);
            AccountInfo info;
            info.total_wallet_balance = std::stod(std::string(doc["totalWalletBalance"].get_string().value()));
            info.available_balance = std::stod(std::string(doc["availableBalance"].get_string().value()));
            info.total_unrealized_profit = std::stod(std::string(doc["totalUnrealizedProfit"].get_string().value()));
            info.total_margin_balance = std::stod(std::string(doc["totalMarginBalance"].get_string().value()));
            return info;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::vector<PositionInfo> get_positions() {
        std::vector<PositionInfo> positions;

        network::HttpRequest req;
        req.method = network::HttpMethod::GET;
        req.path = "/fapi/v2/positionRisk";
        req.sign = true;

        auto response = rest_client_->request(req);
        if (!response.is_success()) {
            return positions;
        }

        try {
            simdjson::padded_string padded(response.body);
            auto doc = parser_.iterate(padded);
            for (auto position : doc.get_array()) {
                double qty = std::stod(std::string(position["positionAmt"].get_string().value()));
                // Use epsilon for float comparison (User Request: "Tuzak 2")
                if (std::abs(qty) < 1e-7) continue;  // Skip empty/dust positions

                PositionInfo info;
                info.symbol = Symbol(position["symbol"].get_string().value());
                info.quantity = Quantity::from_double(std::abs(qty));
                info.side = qty > 0 ? PositionSide::Long : PositionSide::Short;
                info.entry_price = parse_price(position["entryPrice"].get_string().value());
                info.unrealized_profit = std::stod(std::string(position["unRealizedProfit"].get_string().value()));
                info.leverage = std::stod(std::string(position["leverage"].get_string().value()));
                info.liquidation_price = parse_price(position["liquidationPrice"].get_string().value());

                positions.push_back(std::move(info));
            }
        } catch (...) {
            // Parse error, return empty
        }

        return positions;
    }

    std::optional<PositionInfo> get_position(const Symbol& symbol) {
        auto positions = get_positions();
        for (const auto& pos : positions) {
            if (pos.symbol == symbol) {
                return pos;
            }
        }
        return std::nullopt;
    }

    std::vector<AccountTrade> get_account_trades(const Symbol& symbol, int limit) {
        std::vector<AccountTrade> trades;

        network::HttpRequest req;
        req.method = network::HttpMethod::GET;
        req.path = "/fapi/v1/userTrades";
        req.sign = true;

        if (!symbol.empty()) {
            req.query_params["symbol"] = std::string(symbol.view());
        }
        if (limit > 0) {
            req.query_params["limit"] = std::to_string(limit);
        }

        auto response = rest_client_->request(req);
        if (!response.is_success()) {
            return trades;
        }

        try {
            simdjson::padded_string padded(response.body);
            auto doc = parser_.iterate(padded);
            for (auto trade : doc.get_array()) {
                AccountTrade info;
                info.id = trade["id"].get_int64().value();
                info.symbol = Symbol(trade["symbol"].get_string().value());
                info.order_id = trade["orderId"].get_int64().value();
                info.side = parse_side(trade["side"].get_string().value());
                info.price = parse_price(trade["price"].get_string().value());
                info.quantity = parse_quantity(trade["qty"].get_string().value());
                info.realized_pnl = std::stod(std::string(trade["realizedPnl"].get_string().value()));
                info.commission = std::stod(std::string(trade["commission"].get_string().value()));
                info.commission_asset = std::string(trade["commissionAsset"].get_string().value());
                info.time = opus::from_epoch_ms(trade["time"].get_int64().value());
                info.is_buyer = trade["buyer"].get_bool().value();
                info.is_maker = trade["maker"].get_bool().value();

                trades.push_back(std::move(info));
            }
        } catch (...) {
            // Parse error
        }

        return trades;
    }

    std::vector<OrderInfo> get_open_orders(const Symbol& symbol) {
        std::vector<OrderInfo> orders;

        network::HttpRequest req;
        req.method = network::HttpMethod::GET;
        req.path = "/fapi/v1/openOrders";
        req.sign = true;

        if (!symbol.empty()) {
            req.query_params["symbol"] = std::string(symbol.view());
        }

        auto response = rest_client_->request(req);
        if (!response.is_success()) {
            return orders;
        }

        try {
            simdjson::padded_string padded(response.body);
            auto doc = parser_.iterate(padded);
            for (auto order : doc.get_array()) {
                OrderInfo info;
                info.order_id = order["orderId"].get_int64().value();
                info.client_order_id = std::string(order["clientOrderId"].get_string().value());
                info.symbol = Symbol(order["symbol"].get_string().value());
                info.side = parse_side(order["side"].get_string().value());
                info.status = parse_order_status(order["status"].get_string().value());
                info.price = parse_price(order["price"].get_string().value());
                info.quantity = parse_quantity(order["origQty"].get_string().value());
                info.executed_qty = parse_quantity(order["executedQty"].get_string().value());

                orders.push_back(std::move(info));
            }
        } catch (...) {
            // Parse error
        }

        return orders;
    }

    // Helper for formatting doubles
    std::string format_number(double val) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(8) << val;
        std::string s = oss.str();
        // Remove trailing zeros
        s.erase(s.find_last_not_of('0') + 1, std::string::npos);
        // Remove trailing dot if exists
        if (!s.empty() && s.back() == '.') {
            s.pop_back();
        }
        return s;
    }

    std::optional<OrderInfo> place_order(const OrderRequest& request) {
        // Binance migrated conditional orders to Algo Service (Dec 2025)
        // STOP_MARKET and TAKE_PROFIT_MARKET must use /fapi/v1/algoOrder
        bool is_algo = (request.type == OrderType::StopMarket ||
                        request.type == OrderType::TakeProfitMarket);

        network::HttpRequest req;
        req.method = network::HttpMethod::POST;
        req.path = is_algo ? "/fapi/v1/algoOrder" : "/fapi/v1/order";
        
        // DEBUG LOGGING
        {
            std::ostringstream ds;
            ds << "[DEBUG] Placing Order: " << order_type_to_string(request.type) 
               << " " << side_to_string(request.side) 
               << " Qty:" << std::fixed << std::setprecision(4) << request.quantity.to_double()
               << " Path:" << req.path << "\n";
            std::cerr << ds.str();
        }

        req.sign = true;

        req.query_params["symbol"] = std::string(request.symbol.view());
        req.query_params["side"] = side_to_string(request.side);
        req.query_params["type"] = order_type_to_string(request.type);
        
        // Algo orders require algoType=CONDITIONAL
        if (is_algo) {
            req.query_params["algoType"] = "CONDITIONAL";
        }
        
        if (!request.close_position) {
            req.query_params["quantity"] = format_number(request.quantity.to_double());
        }

        if (request.type == OrderType::Limit || request.type == OrderType::StopLimit ||
            request.type == OrderType::TakeProfit) {
            req.query_params["price"] = format_number(request.price.to_double());
            req.query_params["timeInForce"] = tif_to_string(request.time_in_force);
        }
        
        // Algo orders use "triggerPrice", regular orders use "stopPrice"
        if (request.type == OrderType::StopMarket || request.type == OrderType::TakeProfitMarket ||
            request.type == OrderType::StopLimit || request.type == OrderType::TakeProfit) {
            if (request.stop_price.is_valid()) {
                if (is_algo) {
                    req.query_params["triggerPrice"] = format_number(request.stop_price.to_double());
                } else {
                    req.query_params["stopPrice"] = format_number(request.stop_price.to_double());
                }
            }
        }
        if (!request.client_order_id.empty()) {
            if (is_algo) {
                req.query_params["clientAlgoId"] = request.client_order_id;
            } else {
                req.query_params["newClientOrderId"] = request.client_order_id;
            }
        }

        if (request.close_position) {
            req.query_params["closePosition"] = "true";
            // Note: reduceOnly cannot be used with closePosition
        } else if (request.reduce_only) {
            req.query_params["reduceOnly"] = "true";
        }

        last_error_.clear();
        auto response = rest_client_->request(req);
        if (!response.is_success()) {
            std::ostringstream details;
            details << "Order failed (" << response.status_code << "): " << response.body
                    << " | qty=" << request.quantity.to_double();
            if (request.price.is_valid()) {
                details << " price=" << request.price.to_double();
            }
            if (request.stop_price.is_valid()) {
                details << " stop=" << request.stop_price.to_double();
            }
            last_error_ = details.str();
            if (last_error_.size() > 512) last_error_.resize(512);
            return std::nullopt;
        }

        try {
            // simdjson on-demand requires padded string
            simdjson::padded_string padded(response.body);
            auto doc = parser_.iterate(padded);
            OrderInfo info;

            if (is_algo) {
                // Algo Order response uses algoId/clientAlgoId
                info.order_id = doc["algoId"].get_int64().value();
                auto client_id = doc["clientAlgoId"];
                info.client_order_id = std::string(client_id.get_string().value());
                info.symbol = request.symbol;
                info.side = request.side;
                info.status = OrderStatus::New;
                info.price = request.stop_price;
                info.quantity = request.quantity;
                info.executed_qty = Quantity::from_double(0.0);
                std::cerr << "[ORDER] Algo Order placed: algoId=" << info.order_id << "\n";
            } else {
                // Regular Order response
                // IMPORTANT: simdjson on-demand is FORWARD-ONLY.
                // Read fields in the exact order they appear in Binance JSON:
                // orderId → symbol → status → clientOrderId → price → avgPrice → origQty → executedQty
                info.order_id = doc["orderId"].get_int64().value();
                // skip symbol (we already have it from request)
                auto status_field = doc["status"].get_string().value();
                info.status = parse_order_status(status_field);
                info.client_order_id = std::string(doc["clientOrderId"].get_string().value());
                
                // Read price fields in document order
                auto price_str = doc["price"].get_string().value();
                auto avg_price_str = doc["avgPrice"].get_string().value();
                
                // Skip origQty (use request quantity)
                info.executed_qty = parse_quantity(doc["executedQty"].get_string().value());
                
                info.symbol = request.symbol;
                info.side = request.side;
                info.quantity = request.quantity;
                
                // Resolve fill price: prefer avgPrice > price > trade lookup
                double avg_val = std::stod(std::string(avg_price_str));
                double price_val = std::stod(std::string(price_str));
                if (avg_val > 0) {
                    info.price = Price::from_double(avg_val);
                } else if (price_val > 0) {
                    info.price = Price::from_double(price_val);
                } else {
                    // Fallback: query trade API
                    info.price = resolve_fill_price_from_trades(request.symbol, info.order_id);
                }
            }
            return info;
        } catch (const std::exception& e) {
            last_error_ = std::string("Order parse error: ") + e.what();
            std::cerr << "[DEBUG] " << last_error_ << " | body=" << response.body << "\n";
            return std::nullopt;
        }
    }

private:
    // Fallback: look up fill price from recent trades (gives Binance 50ms to propagate)
    Price resolve_fill_price_from_trades(const Symbol& symbol, int64_t order_id) {
        try {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto trades = get_account_trades(symbol, 5);
            for (const auto& trade : trades) {
                if (trade.order_id == order_id) {
                    return trade.price;
                }
            }
        } catch (...) {}
        return Price::from_double(0.0);
    }
public:

    bool cancel_order(const Symbol& symbol, int64_t order_id) {
        network::HttpRequest req;
        req.method = network::HttpMethod::DEL;
        req.path = "/fapi/v1/order";
        req.sign = true;
        req.query_params["symbol"] = std::string(symbol.view());
        req.query_params["orderId"] = std::to_string(order_id);

        auto response = rest_client_->request(req);
        return response.is_success();
    }

    bool cancel_all_orders(const Symbol& symbol) {
        network::HttpRequest req;
        req.method = network::HttpMethod::DEL;
        req.path = "/fapi/v1/allOpenOrders";
        req.sign = true;
        req.query_params["symbol"] = std::string(symbol.view());

        auto response = rest_client_->request(req);
        return response.is_success();
    }

    bool set_leverage(const Symbol& symbol, int leverage) {
        network::HttpRequest req;
        req.method = network::HttpMethod::POST;
        req.path = "/fapi/v1/leverage";
        req.sign = true;
        req.query_params["symbol"] = std::string(symbol.view());
        req.query_params["leverage"] = std::to_string(leverage);

        auto response = rest_client_->request(req);
        return response.is_success();
    }

    std::optional<Price> get_price(const Symbol& symbol) {
        network::HttpRequest req;
        req.method = network::HttpMethod::GET;
        req.path = "/fapi/v1/ticker/price";
        req.query_params["symbol"] = std::string(symbol.view());

        auto response = rest_client_->request(req);
        if (!response.is_success()) {
            return std::nullopt;
        }

        try {
            simdjson::padded_string padded(response.body);
            auto doc = parser_.iterate(padded);
            return parse_price(doc["price"].get_string().value());
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<DepthUpdate> get_depth(const Symbol& symbol, int limit) {
        network::HttpRequest req;
        req.method = network::HttpMethod::GET;
        req.path = "/fapi/v1/depth";
        req.query_params["symbol"] = std::string(symbol.view());
        req.query_params["limit"] = std::to_string(limit);

        auto response = rest_client_->request(req);
        if (!response.is_success()) {
            return std::nullopt;
        }

        try {
            simdjson::padded_string padded(response.body);
            auto doc = parser_.iterate(padded);

            DepthUpdate update;
            update.symbol = symbol;
            update.last_update_id = doc["lastUpdateId"].get_int64().value();
            update.event_time = now();

            for (auto bid : doc["bids"].get_array()) {
                auto arr = bid.get_array();
                auto it = arr.begin();
                auto price = parse_price((*it).get_string().value());
                ++it;
                auto qty = parse_quantity((*it).get_string().value());
                update.bids.push_back({price, qty});
            }

            for (auto ask : doc["asks"].get_array()) {
                auto arr = ask.get_array();
                auto it = arr.begin();
                auto price = parse_price((*it).get_string().value());
                ++it;
                auto qty = parse_quantity((*it).get_string().value());
                update.asks.push_back({price, qty});
            }

            return update;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::vector<Kline> get_klines(const Symbol& symbol, std::string_view interval, int limit) {
        std::vector<Kline> klines;

        network::HttpRequest req;
        req.method = network::HttpMethod::GET;
        req.path = "/fapi/v1/klines";
        req.query_params["symbol"] = std::string(symbol.view());
        req.query_params["interval"] = std::string(interval);
        req.query_params["limit"] = std::to_string(limit);

        auto response = rest_client_->request(req);
        if (!response.is_success()) {
            return klines;
        }

        try {
            simdjson::padded_string padded(response.body);
            auto doc = parser_.iterate(padded);
            for (auto kline_arr : doc.get_array()) {
                auto arr = kline_arr.get_array();
                auto it = arr.begin();

                Kline kline;
                kline.open_time = from_epoch_ms((*it).get_int64().value());
                ++it;
                kline.open = parse_price((*it).get_string().value());
                ++it;
                kline.high = parse_price((*it).get_string().value());
                ++it;
                kline.low = parse_price((*it).get_string().value());
                ++it;
                kline.close = parse_price((*it).get_string().value());
                ++it;
                kline.volume = parse_quantity((*it).get_string().value());
                ++it;
                kline.close_time = from_epoch_ms((*it).get_int64().value());

                klines.push_back(kline);
            }
        } catch (...) {
            // Parse error
        }

        return klines;
    }

    // ========================================================================
    // WebSocket Methods
    // ========================================================================

    void setup_websocket_handlers(BinanceClient* self) {
        ws_client_->on_connect([this, self]() {
            bool was_connected = connected_.exchange(true);
            // Re-subscribe all streams on reconnection
            if (was_connected || !active_subscriptions_.empty()) {
                std::cerr << "[BINANCE] WS connected, re-subscribing "
                          << active_subscriptions_.size() << " stream(s)\n";
                for (const auto& msg : active_subscriptions_) {
                    ws_client_->send(msg);
                }
            }
            // Fire user connect callback (Telegram notification etc.)
            if (self->on_ws_connect_) {
                self->on_ws_connect_();
            }
        });

        ws_client_->on_disconnect([this, self]() {
            connected_ = false;
            std::cerr << "[BINANCE] WS disconnected\n";
        });

        ws_client_->on_message([this, self](std::string_view message) {
            handle_websocket_message(message, self);
        });

        ws_client_->on_error([self](const std::string& error) {
            if (self->on_error_) {
                self->on_error_(error);
            }
        });
    }

    void handle_websocket_message(std::string_view message, BinanceClient* self) {
        try {
            // simdjson requires padded_string for iteration
            simdjson::padded_string padded(message);
            simdjson::ondemand::document doc = parser_.iterate(padded);
            auto event_type = doc["e"].get_string().value();

            if (event_type == "depthUpdate") {
                handle_depth_update(doc, self);
            } else if (event_type == "aggTrade") {
                handle_trade_update(doc, self);
            } else if (event_type == "kline") {
                handle_kline_update(doc, self);
            }
        } catch (...) {
            // Parse error
        }
    }

    void handle_depth_update(simdjson::ondemand::document& doc, BinanceClient*) {
        std::string symbol_str(doc["s"].get_string().value());

        std::lock_guard<std::mutex> lock(callback_mutex_);
        auto it = depth_callbacks_.find(symbol_str);
        if (it == depth_callbacks_.end()) return;

        DepthUpdate update;
        update.symbol = Symbol(symbol_str);
        update.last_update_id = doc["u"].get_int64().value();
        update.event_time = from_epoch_ms(doc["E"].get_int64().value());

        for (auto bid : doc["b"].get_array()) {
            auto arr = bid.get_array();
            auto bit = arr.begin();
            auto price = parse_price((*bit).get_string().value());
            ++bit;
            auto qty = parse_quantity((*bit).get_string().value());
            update.bids.push_back({price, qty});
        }

        for (auto ask : doc["a"].get_array()) {
            auto arr = ask.get_array();
            auto ait = arr.begin();
            auto price = parse_price((*ait).get_string().value());
            ++ait;
            auto qty = parse_quantity((*ait).get_string().value());
            update.asks.push_back({price, qty});
        }

        it->second(update);
    }

    void handle_trade_update(simdjson::ondemand::document& doc, BinanceClient*) {
        std::string symbol_str(doc["s"].get_string().value());

        std::lock_guard<std::mutex> lock(callback_mutex_);
        auto it = trade_callbacks_.find(symbol_str);
        if (it == trade_callbacks_.end()) return;

        TradeUpdate update;
        update.symbol = Symbol(symbol_str);
        update.trade_id = doc["a"].get_int64().value();
        update.price = parse_price(doc["p"].get_string().value());
        update.quantity = parse_quantity(doc["q"].get_string().value());
        update.side = doc["m"].get_bool().value() ? Side::Sell : Side::Buy;
        update.trade_time = from_epoch_ms(doc["T"].get_int64().value());

        it->second(update);
    }

    void handle_kline_update(simdjson::ondemand::document& doc, BinanceClient*) {
        std::string symbol_str(doc["s"].get_string().value());

        std::lock_guard<std::mutex> lock(callback_mutex_);
        auto it = kline_callbacks_.find(symbol_str);
        if (it == kline_callbacks_.end()) return;

        auto k = doc["k"];

        KlineUpdate update;
        update.symbol = Symbol(symbol_str);
        update.is_final = k["x"].get_bool().value();

        update.kline.open_time = from_epoch_ms(k["t"].get_int64().value());
        update.kline.close_time = from_epoch_ms(k["T"].get_int64().value());
        update.kline.open = parse_price(k["o"].get_string().value());
        update.kline.high = parse_price(k["h"].get_string().value());
        update.kline.low = parse_price(k["l"].get_string().value());
        update.kline.close = parse_price(k["c"].get_string().value());
        update.kline.volume = parse_quantity(k["v"].get_string().value());

        it->second(update);
    }

    void subscribe_depth(const Symbol& symbol, DepthCallback callback) {
        std::string sym_lower(symbol.view());
        std::transform(sym_lower.begin(), sym_lower.end(), sym_lower.begin(), ::tolower);

        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            depth_callbacks_[std::string(symbol.view())] = std::move(callback);
        }

        // Send subscribe message (and track for re-subscribe on reconnect)
        std::string msg = R"({"method":"SUBSCRIBE","params":[")" + sym_lower + R"(@depth@100ms"],"id":1})";
        {
            std::lock_guard<std::mutex> lock2(callback_mutex_);
            active_subscriptions_.push_back(msg);
        }
        ws_client_->send(msg);
    }

    void subscribe_trades(const Symbol& symbol, TradeCallback callback) {
        std::string sym_lower(symbol.view());
        std::transform(sym_lower.begin(), sym_lower.end(), sym_lower.begin(), ::tolower);

        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            trade_callbacks_[std::string(symbol.view())] = std::move(callback);
        }

        std::string msg = R"({"method":"SUBSCRIBE","params":[")" + sym_lower + R"(@aggTrade"],"id":2})";
        {
            std::lock_guard<std::mutex> lock2(callback_mutex_);
            active_subscriptions_.push_back(msg);
        }
        ws_client_->send(msg);
    }

    void subscribe_klines(const Symbol& symbol, std::string_view interval, KlineCallback callback) {
        std::string sym_lower(symbol.view());
        std::transform(sym_lower.begin(), sym_lower.end(), sym_lower.begin(), ::tolower);

        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            kline_callbacks_[std::string(symbol.view())] = std::move(callback);
        }

        std::string msg = R"({"method":"SUBSCRIBE","params":[")" + sym_lower + "@kline_" +
                          std::string(interval) + R"("],"id":3})";
        {
            std::lock_guard<std::mutex> lock2(callback_mutex_);
            active_subscriptions_.push_back(msg);
        }
        ws_client_->send(msg);
    }

    void unsubscribe(const Symbol& symbol) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        std::string sym_str(symbol.view());
        depth_callbacks_.erase(sym_str);
        trade_callbacks_.erase(sym_str);
        kline_callbacks_.erase(sym_str);
    }

    void start(BinanceClient* self) {
        setup_websocket_handlers(self);
        ws_client_->connect();
        ws_client_->run_async();
    }

    void stop() {
        ws_client_->stop();
        connected_ = false;
    }

    bool is_connected() const {
        return connected_ && ws_client_->is_connected();
    }

    void set_reconnect_callback(IBinanceClient::ReconnectCallback cb) {
        ws_client_->on_reconnect(std::move(cb));
    }

    [[nodiscard]] const std::string& last_error() const { return last_error_; }
};

// ============================================================================
// BinanceClient Public Interface
// ============================================================================

BinanceClient::BinanceClient(const BinanceConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

BinanceClient::~BinanceClient() {
    stop();
}

std::optional<AccountInfo> BinanceClient::get_account_info() {
    return impl_->get_account_info();
}

std::vector<PositionInfo> BinanceClient::get_positions() {
    return impl_->get_positions();
}

std::optional<PositionInfo> BinanceClient::get_position(const Symbol& symbol) {
    return impl_->get_position(symbol);
}

std::vector<OrderInfo> BinanceClient::get_open_orders(const Symbol& symbol) {
    return impl_->get_open_orders(symbol);
}

std::vector<AccountTrade> BinanceClient::get_account_trades(const Symbol& symbol, int limit) {
    return impl_->get_account_trades(symbol, limit);
}

std::optional<OrderInfo> BinanceClient::place_order(const OrderRequest& request) {
    auto result = impl_->place_order(request);
    if (!result && on_error_) {
        const auto& err = impl_->last_error();
        on_error_(err.empty() ? "Order request failed" : err);
    }
    return result;
}

bool BinanceClient::cancel_order(const Symbol& symbol, int64_t order_id) {
    return impl_->cancel_order(symbol, order_id);
}

bool BinanceClient::cancel_all_orders(const Symbol& symbol) {
    return impl_->cancel_all_orders(symbol);
}

bool BinanceClient::set_leverage(const Symbol& symbol, int leverage) {
    return impl_->set_leverage(symbol, leverage);
}

std::optional<Price> BinanceClient::get_price(const Symbol& symbol) {
    return impl_->get_price(symbol);
}

std::optional<DepthUpdate> BinanceClient::get_depth(const Symbol& symbol, int limit) {
    return impl_->get_depth(symbol, limit);
}

std::vector<Kline> BinanceClient::get_klines(const Symbol& symbol, std::string_view interval, int limit) {
    return impl_->get_klines(symbol, interval, limit);
}

void BinanceClient::subscribe_depth(const Symbol& symbol, DepthCallback callback) {
    impl_->subscribe_depth(symbol, std::move(callback));
}

void BinanceClient::subscribe_trades(const Symbol& symbol, TradeCallback callback) {
    impl_->subscribe_trades(symbol, std::move(callback));
}

void BinanceClient::subscribe_klines(const Symbol& symbol, std::string_view interval, KlineCallback callback) {
    impl_->subscribe_klines(symbol, interval, std::move(callback));
}

void BinanceClient::unsubscribe(const Symbol& symbol) {
    impl_->unsubscribe(symbol);
}

void BinanceClient::start() {
    impl_->start(this);
}

void BinanceClient::on_reconnect(ReconnectCallback callback) {
    impl_->set_reconnect_callback(std::move(callback));
}

void BinanceClient::on_ws_connect(ConnectCallback callback) {
    on_ws_connect_ = std::move(callback);
}

void BinanceClient::stop() {
    impl_->stop();
}

bool BinanceClient::is_connected() const {
    return impl_->is_connected();
}

// ============================================================================
// HMAC-SHA256 Utilities
// ============================================================================

std::string hmac_sha256(std::string_view key, std::string_view message) {
    // Note: Actual implementation is in rest_client.cpp
    // This is a public interface for other modules to use
    unsigned char digest[32];
    unsigned int digest_len = 0;

    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(message.data()),
         message.size(),
         digest, &digest_len);

    std::ostringstream ss;
    for (unsigned int i = 0; i < digest_len; ++i) {
        ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(digest[i]);
    }
    return ss.str();
}

std::string generate_signature(std::string_view secret_key, std::string_view query_string) {
    return hmac_sha256(secret_key, query_string);
}

}  // namespace opus::exchange::binance
