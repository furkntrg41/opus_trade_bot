// ============================================================================
// OPUS TRADE BOT - OBI Live Trading
// ============================================================================
// Order Book Imbalance Strategy with Low-Latency WebSocket Data  
// ============================================================================

#include "opus/core/types.hpp"
#include "opus/core/ring_buffer.hpp"
#include "opus/exchange/binance/client.hpp"
#include "opus/market/order_book.hpp"
#include "opus/strategy/order_book_imbalance.hpp"
#include "opus/strategy/signal_filter.hpp"
#include "opus/risk/risk_manager.hpp"
#include "opus/order/order_manager.hpp"
#include "opus/order/position_tracker.hpp"

#include <yaml-cpp/yaml.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>

namespace {
    std::atomic<bool> g_running{true};

    void signal_handler(int signal) {
        std::cout << "\n[SIGNAL] Received " << signal << ", stopping...\n";
        g_running = false;
    }
}

using namespace opus;

// ============================================================================
// Configuration Loader
// ============================================================================

struct AppConfig {
    std::string api_key;
    std::string secret_key;
    bool testnet = true;
    std::string symbol = "BTCUSDT";
    
    // OBI Settings
    size_t depth_levels = 10;
    double imbalance_threshold = 0.3;
    size_t smoothing_period = 10;
    
    // Risk
    double max_position_pct = 0.05;
    int max_leverage = 5;
};

AppConfig load_config(const std::string& path) {
    AppConfig config;
    
    try {
        YAML::Node yaml = YAML::LoadFile(path);
        
        // Exchange
        if (yaml["exchange"]) {
            config.api_key = yaml["exchange"]["api_key"].as<std::string>("");
            config.secret_key = yaml["exchange"]["secret_key"].as<std::string>("");
            config.testnet = yaml["exchange"]["environment"].as<std::string>("testnet") == "testnet";
        }
        
        // Trading
        if (yaml["trading"] && yaml["trading"]["symbols"]) {
            config.symbol = yaml["trading"]["symbols"][0].as<std::string>("BTCUSDT");
        }
        
        // OBI
        if (yaml["strategy"] && yaml["strategy"]["obi"]) {
            auto obi = yaml["strategy"]["obi"];
            config.depth_levels = obi["depth_levels"].as<size_t>(10);
            config.imbalance_threshold = obi["imbalance_threshold"].as<double>(0.3);
            config.smoothing_period = obi["smoothing_period"].as<size_t>(10);
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Config load failed: " << e.what() << "\n";
    }
    
    return config;
}

// ============================================================================
// OBI Trading Engine
// ============================================================================

class OBITradingEngine {
public:
    explicit OBITradingEngine(const AppConfig& config)
        : config_(config) {
        
        // Initialize OBI signal generator
        strategy::ImbalanceSignalGenerator::Config obi_config;
        obi_config.depth_levels = config.depth_levels;
        obi_config.threshold = config.imbalance_threshold;
        obi_config.smoothing_period = config.smoothing_period;
        
        // Initialize Signal Filter (reduces 1666 -> 3-4 qualified signals)
        strategy::SignalFilterConfig filter_config;
        filter_config.imbalance_threshold = 0.6;        // Production value
        filter_config.high_conviction_threshold = 0.7;  // Instant entry
        filter_config.confirmation_ticks = 3;           // 300ms confirm
        filter_config.cooldown_seconds = 30;            // 30s cooldown
        signal_filter_ = std::make_unique<strategy::SignalFilter>(filter_config);
        
        // Initialize Risk Manager
        risk::RiskConfig risk_config;
        risk_config.max_position_usd = 100.0;
        risk_config.max_orders_per_minute = 2;
        risk_config.stop_loss_pct = 0.25;    // Fee-aware
        risk_config.take_profit_pct = 0.50;  // 2:1 R:R
        risk_manager_ = std::make_unique<risk::RiskManager>(risk_config);
        
        obi_generator_ = std::make_unique<strategy::ImbalanceSignalGenerator>(obi_config);
        
        // Initialize Binance client
        exchange::binance::BinanceConfig binance_config;
        binance_config.api_key = config.api_key;
        binance_config.secret_key = config.secret_key;
        binance_config.testnet = config.testnet;
        
        
        
        client_ = std::make_unique<exchange::binance::BinanceClient>(binance_config);
        order_manager_ = std::make_unique<order::OrderManager>(*client_);
        position_tracker_ = std::make_unique<order::PositionTracker>(*client_);
    }
    
    bool start() {
        std::cout << "========================================\n";
        std::cout << "  OPUS TRADE BOT - OBI Strategy\n";
        std::cout << "  Symbol: " << config_.symbol << "\n";
        std::cout << "  Testnet: " << (config_.testnet ? "YES" : "NO") << "\n";
        std::cout << "  Depth Levels: " << config_.depth_levels << "\n";
        std::cout << "  Threshold: " << config_.imbalance_threshold << "\n";
        std::cout << "========================================\n\n";
        
        // Check API connection
        std::cout << "[INFO] Testing API connection...\n";
        auto account = client_->get_account_info();
        if (!account) {
            std::cerr << "[ERROR] Failed to connect to Binance API\n";
            std::cerr << "[HINT] Check your API keys in config.yaml\n";
            return false;
        }
        
        std::cout << "[OK] Connected! Available Balance: $" 
                  << std::fixed << std::setprecision(2)
                  << account->available_balance << "\n\n";
        
        // Set up WebSocket error callback for diagnostics
        client_->on_error([](const std::string& error) {
            std::cerr << "[BIN_ERROR] " << error << std::endl;
        });
        
        // Start WebSocket connection
        std::cout << "[INFO] Connecting WebSocket to stream.testnet.binancefuture.com...\n";
        client_->start();
        
        // Wait for WebSocket to connect (max 5 seconds)
        std::cout << "[INFO] Waiting for WebSocket handshake...\n";
        std::cout << "[INFO] Press Ctrl+C to stop\n";
        
        // Wait for WebSocket to connect (max 5 seconds)
        std::cout << "[INFO] Waiting for WebSocket handshake...\n";
        for (int i = 0; i < 50 && !client_->is_connected(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (!client_->is_connected()) {
            std::cerr << "[ERROR] WebSocket connection failed after 5 seconds\n";
            std::cerr << "[HINT] Check network/firewall settings\n";
            return false;
        }
        
        std::cout << "[OK] WebSocket connected!\n";
        
        // Subscribe to depth stream
        std::cout << "[INFO] Subscribing to " << config_.symbol << " depth@100ms...\n";
        client_->subscribe_depth(Symbol(config_.symbol), 
            [this](const exchange::binance::DepthUpdate& update) {
                on_depth_update(update);
            });
        
        std::cout << "[OK] Subscribed to depth stream!\n";
        std::cout << "[INFO] Press Ctrl+C to stop\n\n";
        
        return true;
    }
    
    void run() {
        using namespace std::chrono;
        
        auto last_print = steady_clock::now();
        auto last_poll_time = steady_clock::now();
        
        while (g_running) {
            auto now_time = steady_clock::now();
            
            // WebSocket callback handles depth updates - just print stats periodically
            if (duration_cast<seconds>(now_time - last_print).count() >= 5) {
                print_stats();
                last_print = now_time;
            }

            // Smart Polling: Sync positions every 2 seconds IF we think we have exposure
            if (duration_cast<seconds>(now_time - last_poll_time).count() >= 2) {
                if (risk_manager_->open_positions() > 0 || position_tracker_->has_open_position()) {
                    if (position_tracker_->sync_with_exchange()) {
                        risk_manager_->on_position_closed(0.0); // PnL is approx/unknown for now
                        std::cout << "[TRACKER] Position Closed (Sync).\n";
                    }
                }
                last_poll_time = now_time;
            }
            
            // Small sleep - main work done in WebSocket callback thread
            std::this_thread::sleep_for(milliseconds(50));
        }
    }
    
    void stop() {
        std::cout << "\n[INFO] Stopping...\n";
        client_->stop();
        
        // Print final stats
        std::cout << "\n=== Final Statistics ===\n";
        std::cout << "Total Updates:     " << stats_.total_updates << "\n";
        std::cout << "------- Raw Signals -------\n";
        std::cout << "Buy Signals:       " << stats_.buy_signals << "\n";
        std::cout << "Sell Signals:      " << stats_.sell_signals << "\n";
        std::cout << "Total Raw:         " << (stats_.buy_signals + stats_.sell_signals) << "\n";
        std::cout << "--- Qualified (Filtered) ---\n";
        std::cout << "Qualified Buys:    " << stats_.qualified_buys << "\n";
        std::cout << "Qualified Sells:   " << stats_.qualified_sells << "\n";
        std::cout << "Total Qualified:   " << (stats_.qualified_buys + stats_.qualified_sells) << "\n";
        std::cout << "--- Risk Managed Trades ---\n";
        std::cout << "Approved Trades:   " << stats_.approved_trades << "\n";
        std::cout << "Rejected Trades:   " << stats_.rejected_trades << "\n";
        std::cout << "----------------------------\n";
        uint64_t raw_total = stats_.buy_signals + stats_.sell_signals;
        uint64_t qual_total = stats_.qualified_buys + stats_.qualified_sells;
        double filter_rate = raw_total > 0 ? (1.0 - (double)qual_total / raw_total) * 100.0 : 0.0;
        std::cout << "Filter Rate:       " << std::fixed << std::setprecision(1) 
                  << filter_rate << "% filtered out\n";
        std::cout << "Avg Latency:       " << stats_.avg_latency_us << " μs\n";
        std::cout << "============================\n";
    }
    
private:
    void poll_depth() {
        auto start = std::chrono::high_resolution_clock::now();
        
        // Get depth snapshot via REST API
        auto depth = client_->get_depth(Symbol(config_.symbol), 20);
        if (!depth) {
            return;  // Failed to get depth - skip this cycle
        }
        
        // Update order book
        order_book_.clear();
        for (const auto& level : depth->bids) {
            order_book_.update_bid(level.price, level.quantity);
        }
        for (const auto& level : depth->asks) {
            order_book_.update_ask(level.price, level.quantity);
        }
        
        // Get spans for OBI calculation
        auto bids = order_book_.bids(config_.depth_levels);
        auto asks = order_book_.asks(config_.depth_levels);
        
        // Update OBI generator
        obi_generator_->update(bids, asks);
        
        // Record latency
        auto end = std::chrono::high_resolution_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        
        // Update stats
        stats_.total_updates++;
        stats_.last_latency_us = latency;
        stats_.avg_latency_us = (stats_.avg_latency_us * 0.99) + (latency * 0.01);
        
        // Check for trading signal
        if (obi_generator_->is_ready()) {
            auto signal = obi_generator_->signal();
            double signal_value = signal.value();
            
            if (std::abs(signal_value) > 0.0) {
                if (signal_value > 0) {
                    stats_.buy_signals++;
                } else {
                    stats_.sell_signals++;
                }
            }
        }
        
        // Store for display
        last_imbalance_ = obi_generator_->smoothed_imbalance();
    }

    void on_depth_update(const exchange::binance::DepthUpdate& update) {
        auto start = std::chrono::high_resolution_clock::now();
        
        // Update order book
        order_book_.clear();
        for (const auto& level : update.bids) {
            order_book_.update_bid(level.price, level.quantity);
        }
        for (const auto& level : update.asks) {
            order_book_.update_ask(level.price, level.quantity);
        }
        
        // Get spans for OBI calculation
        auto bids = order_book_.bids(config_.depth_levels);
        auto asks = order_book_.asks(config_.depth_levels);
        
        // Update OBI generator
        obi_generator_->update(bids, asks);
        
        // Record latency
        auto end = std::chrono::high_resolution_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        
        // Update stats
        stats_.total_updates++;
        stats_.last_latency_us = latency;
        stats_.avg_latency_us = (stats_.avg_latency_us * 0.99) + (latency * 0.01);
        
        // Check for trading signal
        if (obi_generator_->is_ready()) {
            double imbalance = obi_generator_->smoothed_imbalance();
            
            // Calculate spread percentage
            const auto* best_bid = order_book_.best_bid();
            const auto* best_ask = order_book_.best_ask();
            double spread_pct = 0.0;
            if (best_bid && best_ask && best_bid->price.to_double() > 0) {
                spread_pct = (best_ask->price.to_double() - best_bid->price.to_double()) 
                           / best_bid->price.to_double() * 100.0;
            }
            
            // Count raw signals (any imbalance)
            if (std::abs(imbalance) > config_.imbalance_threshold) {
                if (imbalance > 0) stats_.buy_signals++;
                else stats_.sell_signals++;
            }
            
            // Apply SignalFilter for qualified signals
            auto filtered = signal_filter_->filter(
                imbalance, 
                spread_pct,
                best_bid ? best_bid->price.to_double() : 0.0,
                best_ask ? best_ask->price.to_double() : 0.0
            );
            
            if (filtered.type != strategy::SignalFilter::SignalType::None) {
                bool is_long = (filtered.type == strategy::SignalFilter::SignalType::Buy);
                double price = is_long ? (best_ask ? best_ask->price.to_double() : 0) 
                                     : (best_bid ? best_bid->price.to_double() : 0);
                
                if (is_long) stats_.qualified_buys++;
                else stats_.qualified_sells++;
                
                // Risk Check
                auto decision = risk_manager_->can_trade(price, is_long);
                
                if (decision.decision == risk::RiskManager::TradeDecision::Approved) {
                    stats_.approved_trades++;
                    
                    std::cout << "\n[EXEC] Placing Bracket Order..."
                              << " | Price: " << price
                              << " | Size: $" << decision.position_size_usd
                              << "\n";

                    Price sl_price = Price::from_double(decision.stop_loss_price);
                    Price tp_price = Price::from_double(decision.take_profit_price);

                    auto result = order_manager_->place_bracket_order(
                        Symbol(config_.symbol),
                        is_long ? Side::Buy : Side::Sell,
                        Quantity::from_usd_value(decision.position_size_usd, price),
                        sl_price,
                        tp_price
                    );

                    if (result.entry_order) {
                        risk_manager_->on_order_placed(); 
                        std::cout << "[EXEC] Entry Filled: " << result.entry_order->price.to_double() << "\n";
                        
                        // Log SL/TP status
                        if (result.stop_loss_order) std::cout << "[EXEC] SL Placed: " << decision.stop_loss_price.to_double() << "\n";
                        else std::cerr << "[EXEC] SL Failed!\n";
                        
                        if (result.take_profit_order) std::cout << "[EXEC] TP Placed: " << decision.take_profit_price.to_double() << "\n";
                        else std::cerr << "[EXEC] TP Failed!\n";

                    } else {
                        std::cerr << "[EXEC] Entry Order Failed!\n";
                    }
                } else {
                    stats_.rejected_trades++;
                    std::cout << "\n[TRADE REJECTED] Reason: " << decision.reason << "\n";
                }
            }
        }
        
        // Store for display
        last_imbalance_ = obi_generator_->smoothed_imbalance();
    }
    
    void print_stats() {
        const auto* best_bid = order_book_.best_bid();
        const auto* best_ask = order_book_.best_ask();
        
        if (!best_bid || !best_ask) {
            std::cout << "[WAIT] No depth data yet...\n";
            return;
        }
        
        std::cout << "[LIVE] "
                  << config_.symbol << " | "
                  << "Bid: " << std::fixed << std::setprecision(2) 
                  << best_bid->price.to_double() << " | "
                  << "Ask: " << best_ask->price.to_double() << " | "
                  << "Imb: " << std::setprecision(3) << std::showpos << last_imbalance_ << std::noshowpos << " | "
                  << "Upd: " << stats_.total_updates << " | "
                  << "Lat: " << std::setprecision(0) << stats_.avg_latency_us << "μs"
                  << "\n";
    }
    
    // Config & Components
    AppConfig config_;
    std::unique_ptr<exchange::binance::BinanceClient> client_;
    market::OrderBook order_book_;
    std::unique_ptr<strategy::ImbalanceSignalGenerator> obi_generator_;
    std::unique_ptr<strategy::SignalFilter> signal_filter_;
    std::unique_ptr<risk::RiskManager> risk_manager_;
    std::unique_ptr<order::OrderManager> order_manager_;
    std::unique_ptr<order::PositionTracker> position_tracker_;
    
    // State
    double last_imbalance_ = 0.0;
    
    // Statistics
    struct Stats {
        uint64_t total_updates = 0;
        uint64_t buy_signals = 0;          // Raw signals
        uint64_t sell_signals = 0;         // Raw signals
        uint64_t qualified_buys = 0;       // After filter
        uint64_t qualified_sells = 0;      // After filter
        uint64_t approved_trades = 0;      // Passed risk check
        uint64_t rejected_trades = 0;      // Failed risk check
        int64_t last_latency_us = 0;
        double avg_latency_us = 0.0;
    } stats_;
};

// ============================================================================
// Main Entry Point
// ============================================================================

int main(int argc, char* argv[]) {
    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // Parse config path
    std::string config_path = "config/config.yaml";
    if (argc > 1) {
        config_path = argv[1];
    }
    
    std::cout << "[INFO] Loading config from: " << config_path << "\n";
    
    // Load configuration
    auto config = load_config(config_path);
    
    if (config.api_key.empty() || config.api_key.find("BURAYA") != std::string::npos) {
        std::cerr << "\n[ERROR] API key not configured!\n";
        std::cerr << "[HINT] Edit config/config.yaml and add your Testnet API keys\n";
        std::cerr << "       Get keys from: https://testnet.binancefuture.com\n\n";
        return 1;
    }
    
    // Create and run engine
    OBITradingEngine engine(config);
    
    if (!engine.start()) {
        return 1;
    }
    
    engine.run();
    engine.stop();
    
    return 0;
}
