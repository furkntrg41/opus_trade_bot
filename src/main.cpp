// ============================================================================
// OPUS TRADE BOT - OBI Live Trading (Event-Driven Architecture)
// ============================================================================
// Order Book Imbalance Strategy with Reactor Pattern Event Loop
//
// Architecture:
//   [WS Thread] --push--> [SPSC Ring Buffer] --drain--> [EventLoop]
//                                                          │
//                                                    ┌─────▼─────┐
//                                                    │ OBI Engine │
//                                                    │ Signal→Risk│
//                                                    │ →Order Mgr │
//                                                    └───────────┘
// ============================================================================

#define _CRT_SECURE_NO_WARNINGS
#include "opus/core/types.hpp"
#include "opus/core/ring_buffer.hpp"
#include "opus/core/event.hpp"
#include "opus/core/event_loop.hpp"
#include "opus/core/message_bus.hpp"
#include "opus/exchange/binance/client.hpp"
#include "opus/exchange/mock_client.hpp"
#include "opus/market/order_book.hpp"
#include "opus/strategy/order_book_imbalance.hpp"
#include "opus/strategy/signal_filter.hpp"
#include "opus/risk/risk_manager.hpp"
#include "opus/order/order_manager.hpp"
#include "opus/order/position_tracker.hpp"
#include "opus/market/recorder.hpp"
#include "opus/market/replay_engine.hpp"
#include "opus/market/multi_symbol_replay.hpp"
#include "opus/strategy/arbitrage/arbitrage_engine.hpp"
#include "opus/strategy/arbitrage/arb_execution_manager.hpp"
#include "opus/network/telegram_notifier.hpp"

#include <yaml-cpp/yaml.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>

namespace {
    std::atomic<bool> g_running{true};

    void signal_handler(int signal) {
        std::cout << "\n[SIGNAL] Received " << signal << ", stopping...\n";
        g_running = false;
    }

    double round_quantity_for_obi(double qty, double price) noexcept {
        double step;
        if (price >= 10000.0)      step = 0.001;
        else if (price >= 1000.0)  step = 0.01;
        else if (price >= 100.0)   step = 0.1;
        else if (price >= 10.0)    step = 1.0;
        else                        step = 10.0;

        return std::floor(qty / step) * step;
    }

    double round_price(double price) noexcept {
        // Round to 1 decimal place (safe for BTCUSDT)
        return std::floor(price * 10.0 + 0.5) / 10.0;
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
    bool replay_mode = false;
    bool arb_backtest_mode = false;      // --mode arb-backtest
    std::string replay_file;           // .bin file for replay mode
    std::vector<std::string> arb_replay_files; // Multi-file for arb backtest
    double replay_speed = 0.0;          // 0 = max speed
    size_t synthetic_ticks = 10000;     // Ticks for synthetic arb backtest
    std::string symbol = "BTCUSDT";
    bool trading_enabled = true;
    
    // OBI Settings
    size_t depth_levels = 10;
    double imbalance_threshold = 0.3;
    size_t smoothing_period = 10;
    
    // Risk
    double max_position_pct = 0.05;
    int max_leverage = 5;

    // Arbitrage Module
    bool arbitrage_enabled = false;
    std::vector<std::pair<std::string, std::string>> arb_pairs;  // {leg_a, leg_b}
    double arb_entry_z = 2.0;
    double arb_exit_z = 0.5;
    double arb_stop_z = 3.5;
    size_t arb_lookback = 300;
    double arb_max_notional = 200.0;
    double arb_min_correlation = 0.70;

    // Arbitrage Execution
    double arb_exec_min_notional = 10.0;
    double arb_exec_max_notional = 500.0;
    double arb_exec_max_slippage = 0.10;
    uint32_t arb_exec_max_concurrent = 3;
    int64_t arb_exec_cooldown_ms = 60000;
    double arb_exec_max_total_notional = 2000.0;
    double arb_exec_max_daily_loss = 2.0;

    // Telegram
    network::TelegramConfig telegram;
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
        if (yaml["trading"]) {
            config.trading_enabled = yaml["trading"]["enabled"].as<bool>(true);
            if (yaml["trading"]["symbols"]) {
                config.symbol = yaml["trading"]["symbols"][0].as<std::string>("BTCUSDT");
            }
        }
        
        // OBI
        if (yaml["strategy"] && yaml["strategy"]["obi"]) {
            auto obi = yaml["strategy"]["obi"];
            config.depth_levels = obi["depth_levels"].as<size_t>(10);
            config.imbalance_threshold = obi["imbalance_threshold"].as<double>(0.3);
            config.smoothing_period = obi["smoothing_period"].as<size_t>(10);
        }
        
        // Arbitrage
        if (yaml["strategy"] && yaml["strategy"]["arbitrage"]) {
            auto arb = yaml["strategy"]["arbitrage"];
            config.arbitrage_enabled = arb["enabled"].as<bool>(false);
            config.arb_entry_z = arb["entry_z"].as<double>(2.0);
            config.arb_exit_z = arb["exit_z"].as<double>(0.5);
            config.arb_stop_z = arb["stop_z"].as<double>(3.5);
            config.arb_lookback = arb["lookback"].as<size_t>(300);
            config.arb_max_notional = arb["max_notional_usd"].as<double>(200.0);
            config.arb_min_correlation = arb["min_correlation"].as<double>(0.70);
            if (arb["pairs"]) {
                for (const auto& p : arb["pairs"]) {
                    config.arb_pairs.emplace_back(
                        p["leg_a"].as<std::string>(),
                        p["leg_b"].as<std::string>());
                }
            }

            // Execution settings
            if (arb["execution"]) {
                auto exec = arb["execution"];
                config.arb_exec_min_notional = exec["min_notional_usd"].as<double>(120.0);
                config.arb_exec_max_notional = exec["max_notional_usd"].as<double>(600.0);
                config.arb_exec_max_slippage = exec["max_slippage_pct"].as<double>(0.10);
                config.arb_exec_max_concurrent = exec["max_concurrent_positions"].as<uint32_t>(3);
                config.arb_exec_cooldown_ms = exec["pair_cooldown_ms"].as<int64_t>(60000);
                config.arb_exec_max_total_notional = exec["max_total_notional"].as<double>(2000.0);
                config.arb_exec_max_daily_loss = exec["max_daily_loss_pct"].as<double>(2.0);
            }
        }

        // Telegram
        if (yaml["telegram"]) {
            auto tg = yaml["telegram"];
            config.telegram.enabled    = tg["enabled"].as<bool>(false);
            config.telegram.bot_token  = tg["bot_token"].as<std::string>("");
            config.telegram.chat_id    = tg["chat_id"].as<std::string>("");
            config.telegram.max_retries = tg["max_retries"].as<int>(3);
            config.telegram.rate_limit  = tg["rate_limit"].as<int>(25);
            config.telegram.dedup_window = tg["dedup_window"].as<int>(60);
        }

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Config load failed: " << e.what() << "\n";
    }
    
    return config;
}

// ============================================================================
// OBI Trading Engine (Event-Driven)
// ============================================================================
// Implements IEventHandler to receive events from the EventLoop.
// In LIVE mode: WS → MessageBus → RingBuffer → EventLoop → this handler
// In REPLAY mode: ReplayEngine → MockClient → direct callback (no ring buffer)

class OBITradingEngine : public IEventHandler {
public:
    explicit OBITradingEngine(const AppConfig& config)
        : config_(config) {
        
        // Initialize Telegram Notifier
        telegram_ = std::make_unique<network::TelegramNotifier>(config.telegram);

        // Initialize OBI signal generator
        strategy::ImbalanceSignalGenerator::Config obi_config;
        obi_config.depth_levels = config.depth_levels;
        obi_config.threshold = config.imbalance_threshold;
        obi_config.smoothing_period = config.smoothing_period;
        
        // Initialize Signal Filter
        strategy::SignalFilterConfig filter_config;
        filter_config.imbalance_threshold = config.imbalance_threshold; 
        signal_filter_ = std::make_unique<strategy::SignalFilter>(filter_config);

        // Initialize Risk Manager
        opus::risk::RiskConfig risk_config;
        risk_config.max_position_usd = 300.0;
        risk_config.max_orders_per_minute = 2;
        risk_config.stop_loss_pct = 0.25;
        risk_config.take_profit_pct = 0.50;
        risk_manager_ = std::make_unique<opus::risk::RiskManager>(risk_config);
        
        obi_generator_ = std::make_unique<strategy::ImbalanceSignalGenerator>(obi_config);

        // Initialize Binance client (Mock or Real)
        if (config.replay_mode || config.arb_backtest_mode) {
            exchange::MockClient::Config mock_config;
            mock_config.initial_balance = 10000.0;
            auto mock = std::make_unique<exchange::MockClient>(mock_config);
            mock_client_ptr_ = mock.get();
            client_ = std::move(mock);
            std::cout << "[INFO] Initialized MOCK Exchange Client ($10,000 balance)\n";
        } else {
            exchange::binance::BinanceConfig binance_config;
            binance_config.api_key = config.api_key;
            binance_config.secret_key = config.secret_key;
            binance_config.testnet = config.testnet;
            client_ = std::make_unique<exchange::binance::BinanceClient>(binance_config);

            // Create EventLoop + MessageBus for LIVE mode
            event_loop_ = std::make_unique<EventLoop>(EventLoopConfig{});
            message_bus_ = std::make_unique<MessageBus>(event_loop_->ring_buffer());
            event_loop_->set_handler(this);
        }
        
        order_manager_ = std::make_unique<order::OrderManager>(*client_);
        position_tracker_ = std::make_unique<order::PositionTracker>(*client_);

        // Initialize Arbitrage Engine + Execution Manager (if enabled)
        if (config.arbitrage_enabled && !config.arb_pairs.empty()) {
            using namespace strategy::arbitrage;
            ArbitrageEngineConfig arb_config;
            for (const auto& [a, b] : config.arb_pairs) {
                ArbitragePairConfig pc;
                pc.leg_a = Symbol(a);
                pc.leg_b = Symbol(b);
                pc.entry_z = config.arb_entry_z;
                pc.exit_z  = config.arb_exit_z;
                pc.stop_z  = config.arb_stop_z;
                pc.lookback_period = config.arb_lookback;
                pc.max_notional_usd = config.arb_max_notional;
                pc.min_correlation = config.arb_min_correlation;
                arb_config.pairs.push_back(pc);
            }
            arb_engine_ = std::make_unique<ArbitrageEngine>(arb_config);

            // Initialize Execution Manager
            ArbExecutionConfig exec_config;
            exec_config.min_notional_usd          = config.arb_exec_min_notional;
            exec_config.max_notional_usd           = config.arb_exec_max_notional;
            exec_config.max_slippage_pct           = config.arb_exec_max_slippage;
            exec_config.max_concurrent_positions   = config.arb_exec_max_concurrent;
            exec_config.pair_cooldown_ms           = config.arb_exec_cooldown_ms;
            exec_config.max_total_notional         = config.arb_exec_max_total_notional;
            exec_config.max_daily_loss_pct         = config.arb_exec_max_daily_loss;

            arb_exec_ = std::make_unique<ArbExecutionManager>(
                exec_config, *order_manager_, *client_);

            // Enable backtest mode on risk guard (skip stale data check, use event timestamps)
            if (config.arb_backtest_mode) {
                arb_exec_->set_backtest_mode(true);
            }

            // Wire: ArbitrageEngine → ArbExecutionManager (entry)
            arb_engine_->on_opportunity(
                [this](const ArbitrageOpportunity& opp, const std::string& pair_key) {
                    arb_exec_->execute_opportunity(opp, pair_key);
                });

            // Wire: ArbitrageEngine → ArbExecutionManager (exit)
            arb_engine_->on_exit(
                [this](const std::string& pair_key, const char* reason) {
                    arb_exec_->execute_exit(pair_key, reason);
                });

            // Wire: ArbExecutionManager → ArbitrageEngine (fill confirmations)
            arb_exec_->on_entry_filled(
                [this](const std::string& pair_key,
                       ArbitrageDirection direction,
                       double z_score, double spread,
                       double price_a, double price_b,
                       double qty_a, double qty_b) {
                    arb_engine_->on_position_opened(
                        pair_key, direction, z_score, spread,
                        price_a, price_b, qty_a, qty_b);
                    // Telegram notification
                    if (telegram_) {
                        std::string dir_str = (direction == ArbitrageDirection::LongA_ShortB)
                                              ? "LONG_A/SHORT_B" : "SHORT_A/LONG_B";
                        // telegram_->notify_arb_entry(pair_key, dir_str, z_score,
                        //                             qty_a, price_a, qty_b, price_b);
                    }
                });

            arb_exec_->on_exit_filled(
                [this](const std::string& pair_key, double pnl_pct) {
                    arb_engine_->on_position_closed(pair_key, pnl_pct);
                    if (telegram_) telegram_->notify_arb_exit(pair_key, "filled", pnl_pct);
                });
        }

        // Initialize Data Recorder (live mode only)
        if (!config.replay_mode && !config.arb_backtest_mode) {
            auto now_time = std::chrono::system_clock::now();
            auto now_t = std::chrono::system_clock::to_time_t(now_time);
            std::stringstream ss;
            ss << "market_data_" << std::put_time(std::localtime(&now_t), "%Y%m%d_%H%M%S") << ".bin";
            recorder_ = std::make_unique<market::DataRecorder>(ss.str(), Symbol(config.symbol));
        }
    }
    
    bool start() {
        std::cout << "========================================\n";
        std::cout << "  OPUS TRADE BOT - OBI Strategy\n";
        std::cout << "  Mode:   " << (config_.arb_backtest_mode ? "ARB-BACKTEST"
                     : config_.replay_mode ? "REPLAY" : "LIVE (Event-Driven)") << "\n";
        std::cout << "  Symbol: " << config_.symbol << "\n";
        if (!config_.replay_mode && !config_.arb_backtest_mode)
            std::cout << "  Testnet: " << (config_.testnet ? "YES" : "NO") << "\n";
        std::cout << "  Depth Levels: " << config_.depth_levels << "\n";
        std::cout << "  Threshold: " << config_.imbalance_threshold << "\n";
        std::cout << "========================================\n\n";

        // ================================================================
        // ARB-BACKTEST MODE: MultiSymbolReplay with synthetic or real data
        // ================================================================
        if (config_.arb_backtest_mode && mock_client_ptr_) {
            if (!arb_engine_) {
                std::cerr << "[ERROR] Arb-backtest requires arbitrage to be enabled with pairs\n";
                return false;
            }

            // Subscribe to ALL arb symbols on MockClient
            auto tracked = arb_engine_->tracked_symbols();
            for (const auto& sym : tracked) {
                mock_client_ptr_->subscribe_depth(Symbol(sym),
                    [this](const exchange::binance::DepthUpdate& update) {
                        // Build DepthEvent for ArbitrageEngine
                        DepthEvent de{};
                        std::string sym_str(update.symbol.view());
                        std::memcpy(de.symbol, sym_str.c_str(),
                                    std::min(sym_str.size(), size_t(15)));
                        de.timestamp_ms = to_epoch_ms(update.event_time);
                        de.sequence = static_cast<uint32_t>(update.last_update_id);
                        de.bid_count = static_cast<uint16_t>(
                            std::min(update.bids.size(), size_t(20)));
                        de.ask_count = static_cast<uint16_t>(
                            std::min(update.asks.size(), size_t(20)));
                        for (uint16_t i = 0; i < de.bid_count; ++i) {
                            de.bids[i].price_raw = update.bids[i].price.raw();
                            de.bids[i].quantity_raw = update.bids[i].quantity.raw();
                        }
                        for (uint16_t i = 0; i < de.ask_count; ++i) {
                            de.asks[i].price_raw = update.asks[i].price.raw();
                            de.asks[i].quantity_raw = update.asks[i].quantity.raw();
                        }
                        arb_engine_->on_depth_event(de);

                        // Also run OBI on primary symbol
                        if (update.symbol == Symbol(config_.symbol)) {
                            handle_depth_update(update);
                        }
                    });
            }

            // Build MultiSymbolReplay
            market::MultiReplayConfig mr_config;
            mr_config.speed_multiplier = config_.replay_speed;
            mr_config.verbose = true;

            if (!config_.arb_replay_files.empty()) {
                // File-based mode
                mr_config.filepaths = config_.arb_replay_files;
                mr_config.synthetic = false;
            } else {
                // Synthetic mode (default for arb-backtest)
                mr_config.synthetic = true;
                mr_config.synthetic_ticks = config_.synthetic_ticks;
                mr_config.synthetic_interval_ms = 100;
            }

            multi_replay_ = std::make_unique<market::MultiSymbolReplay>(
                mr_config, *mock_client_ptr_);

            // If synthetic, configure the pair
            if (mr_config.synthetic && !config_.arb_pairs.empty()) {
                market::SyntheticPairConfig pair_cfg;
                pair_cfg.symbol_a = Symbol(config_.arb_pairs[0].first);
                pair_cfg.symbol_b = Symbol(config_.arb_pairs[0].second);
                // Use reasonable defaults for BTC/ETH style pairs
                pair_cfg.start_price_a = 50000.0;
                pair_cfg.start_price_b = 3000.0;
                pair_cfg.correlation = 0.85;
                pair_cfg.volatility_a = 0.0003;
                pair_cfg.volatility_b = 0.0005;
                synthetic_pair_config_ = pair_cfg;
            }

            auto account = client_->get_account_info();
            std::cout << "[OK] Mock Exchange Ready. Balance: $"
                      << std::fixed << std::setprecision(2)
                      << (account ? account->available_balance : 0.0) << "\n\n";
            return true;
        }

        // ================================================================
        // REPLAY MODE: Setup ReplayEngine (direct callback, no ring buffer)
        // ================================================================
        if (config_.replay_mode) {
            if (config_.replay_file.empty()) {
                std::cerr << "[ERROR] Replay mode requires --file <path.bin>\n";
                return false;
            }

            mock_client_ptr_->subscribe_depth(Symbol(config_.symbol),
                [this](const exchange::binance::DepthUpdate& update) {
                    handle_depth_update(update);
                });

            market::ReplayEngine::Config replay_config;
            replay_config.filepath = config_.replay_file;
            replay_config.speed_multiplier = config_.replay_speed;
            replay_config.verbose = true;

            replay_engine_ = std::make_unique<market::ReplayEngine>(
                replay_config, *mock_client_ptr_);

            auto account = client_->get_account_info();
            std::cout << "[OK] Mock Exchange Ready. Balance: $"
                      << std::fixed << std::setprecision(2)
                      << (account ? account->available_balance : 0.0) << "\n\n";
            return true;
        }

        // ================================================================
        // LIVE MODE: Connect + Subscribe via MessageBus
        // ================================================================
        std::cout << "[INFO] Testing API connection...\n";
        auto account = client_->get_account_info();
        if (!account) {
            std::cerr << "[ERROR] Failed to connect to Binance API\n";
            std::cerr << "[HINT] Check your API keys in config.yaml\n";
            return false;
        }
        
        std::cout << "[OK] Connected! Balance: $" 
                  << std::fixed << std::setprecision(2)
                  << account->available_balance << "\n\n";
        
        client_->on_error([this](const std::string& error) {
            std::cerr << "[BIN_ERROR] " << error << std::endl;
            if (error.find("Operation canceled") != std::string::npos) {
                return;  // Normal WS disconnect/reconnect noise
            }
            if (telegram_) telegram_->notify_error(error);
        });
        
        std::cout << "[INFO] Connecting WebSocket...\n";
        
        // Wire reconnect notifications (before start so callback is in place)
        client_->on_reconnect([this](size_t attempt, std::chrono::seconds delay) {
            std::cout << "[WS] Reconnect attempt " << attempt
                      << ", backoff " << delay.count() << "s\n";
            // if (telegram_) telegram_->notify_reconnect(
            //     static_cast<int>(attempt), 0 /* unlimited */);
        });

        // Wire WS connected notification (fires on every connect, including reconnects)
        client_->on_ws_connect([this]() {
            // if (telegram_) telegram_->notify_connected();
        });

        client_->start();
        
        std::cout << "[INFO] Waiting for WebSocket handshake...\n";
        for (int i = 0; i < 50 && !client_->is_connected(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (!client_->is_connected()) {
            std::cerr << "[ERROR] WebSocket connection failed after 5 seconds\n";
            return false;
        }
        
        std::cout << "[OK] WebSocket connected!\n";
        
        // Subscribe depth: WS thread pushes into MessageBus (lock-free)
        // EventLoop drains ring buffer and calls on_depth_event() on THIS thread
        auto depth_cb = [this](const exchange::binance::DepthUpdate& update) {
            // --- HOT PATH: WS thread (network I/O) ---
            if (recorder_) recorder_->write(update);
            message_bus_->publish_depth(update);
        };

        // Primary symbol (OBI strategy)
        std::cout << "[INFO] Subscribing to " << config_.symbol
                  << " depth@100ms via MessageBus...\n";
        client_->subscribe_depth(Symbol(config_.symbol), depth_cb);

        // Additional symbols for Arbitrage Engine
        if (arb_engine_) {
            auto tracked = arb_engine_->tracked_symbols();
            for (const auto& sym : tracked) {
                if (sym == config_.symbol) continue;  // Already subscribed
                std::cout << "[INFO] Subscribing to " << sym
                          << " depth@100ms (Arbitrage)...\n";
                client_->subscribe_depth(Symbol(sym), depth_cb);
            }
        }
        
        std::cout << "[OK] Event-Driven pipeline ready!\n";
        std::cout << "[INFO] WS Thread -> SPSC Ring Buffer -> EventLoop -> Strategy\n";
        if (arb_engine_)
            std::cout << "[INFO] Arbitrage Engine active: "
                      << config_.arb_pairs.size() << " pair(s)\n";
        std::cout << "[INFO] Press Ctrl+C to stop\n\n";

        // --- Telegram: startup notification ---
        if (telegram_) {
            std::string mode_str = config_.testnet ? "LIVE (Testnet)" : "LIVE (Mainnet)";
            if (config_.arbitrage_enabled) mode_str += " + Arb";
            telegram_->notify_startup(mode_str, config_.symbol);
            if (!config_.trading_enabled) {
                telegram_->send("🟡 *WATCH ONLY*\nTrading disabled; signals ignored.",
                                 network::TelegramPriority::WARNING,
                                 "watch_only");
            }
        }
        
        return true;
    }
    
    void run() {
        // ================================================================
        // ARB-BACKTEST MODE: MultiSymbolReplay drives synchronously
        // ================================================================
        if (config_.arb_backtest_mode && multi_replay_) {
            std::cout << "[INFO] Starting arbitrage backtest...\n";
            if (synthetic_pair_config_.has_value()) {
                if (!multi_replay_->run_synthetic(*synthetic_pair_config_)) {
                    std::cerr << "[ERROR] Synthetic replay failed\n";
                }
            } else {
                if (!multi_replay_->run()) {
                    std::cerr << "[ERROR] Multi-symbol replay failed\n";
                }
            }
            return;
        }

        // ================================================================
        // REPLAY MODE: ReplayEngine drives synchronously
        // ================================================================
        if (config_.replay_mode && replay_engine_) {
            std::cout << "[INFO] Starting replay...\n";
            if (!replay_engine_->run()) {
                std::cerr << "[ERROR] Replay failed or was interrupted\n";
            }
            return;
        }

        // ================================================================
        // LIVE MODE: EventLoop reactor (replaces while+sleep)
        // ================================================================
        if (event_loop_) {
            // Run the Asio reactor - blocks until stop() or SIGINT
            // This is the ONLY thread that processes strategy logic
            event_loop_->run();
        }
    }
    
    void stop() {
        std::cout << "\n[INFO] Stopping...\n";

        // Telegram shutdown notification (before disconnecting!)
        if (telegram_) telegram_->notify_shutdown("Ctrl+C / SIGINT");

        // Stop EventLoop first (if live mode)
        if (event_loop_) {
            event_loop_->stop();
        }

        client_->stop();
        
        // Print OBI strategy stats
        std::cout << "\n=== OBI Strategy Statistics ===\n";
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
        std::cout << "Avg Latency:       " << stats_.avg_latency_us << " us\n";
        std::cout << "============================\n";

        // Arbitrage Engine stats
        if (arb_engine_) {
            std::cout << "\n=== Arbitrage Statistics ===\n";
            for (const auto& key : arb_engine_->pair_keys()) {
                const auto& st = arb_engine_->stats(key);
                std::cout << "Pair: " << key << "\n";
                std::cout << "  Ticks:         " << st.total_ticks << "\n";
                std::cout << "  Opportunities: " << st.opportunities << "\n";
                std::cout << "  Entries:       " << st.entries << "\n";
                std::cout << "  Exits (Profit):" << st.exits_profit << "\n";
                std::cout << "  Exits (Stop):  " << st.exits_stop << "\n";
                std::cout << "  Exits (Decor): " << st.exits_decorrelation << "\n";
                std::cout << "  Total PnL:     " << std::fixed << std::setprecision(3)
                          << std::showpos << st.total_pnl_pct << "%" << std::noshowpos << "\n";
                std::cout << "  Max |Z|:       " << st.max_z_score << "\n";
            }
            std::cout << "============================\n";
        }

        // Arbitrage Execution stats
        if (arb_exec_) {
            arb_exec_->print_stats();
        }

        // EventLoop / MessageBus stats (live mode)
        if (event_loop_) {
            std::cout << "\n=== Event Loop Statistics ===\n";
            std::cout << "Events Processed:  " << event_loop_->events_processed() << "\n";
            if (message_bus_) {
                std::cout << "Events Published:  " << message_bus_->events_published() << "\n";
                std::cout << "Events Dropped:    " << message_bus_->events_dropped() << "\n";
                std::cout << "Drop Rate:         " << std::setprecision(4)
                          << (message_bus_->drop_rate() * 100.0) << "%\n";
            }
            std::cout << "============================\n";
        }

        // Backtest P&L Report (replay mode)
        if (config_.replay_mode && mock_client_ptr_) {
            auto account = mock_client_ptr_->get_account_info();
            if (account) {
                double pnl = account->total_wallet_balance - 10000.0;
                double pnl_pct = (pnl / 10000.0) * 100.0;
                std::cout << "\n=== Backtest P&L Report ===\n";
                std::cout << "Initial Balance:   $10,000.00\n";
                std::cout << "Final Balance:     $" << std::fixed << std::setprecision(2)
                          << account->total_wallet_balance << "\n";
                std::cout << "Net P&L:           $" << std::showpos << pnl
                          << " (" << std::setprecision(3) << pnl_pct << "%)" 
                          << std::noshowpos << "\n";
                std::cout << "Unrealized P&L:    $" << account->total_unrealized_profit << "\n";
                std::cout << "Open Positions:    " << account->positions.size() << "\n";

                auto trades = mock_client_ptr_->get_account_trades(Symbol{}, 1000);
                double total_commission = 0;
                int wins = 0, losses = 0;
                double gross_profit = 0, gross_loss = 0;
                for (const auto& t : trades) {
                    total_commission += t.commission;
                    if (t.realized_pnl > 0) { wins++; gross_profit += t.realized_pnl; }
                    else if (t.realized_pnl < 0) { losses++; gross_loss += t.realized_pnl; }
                }
                std::cout << "Total Trades:      " << trades.size() << "\n";
                std::cout << "Wins / Losses:     " << wins << " / " << losses << "\n";
                if (wins + losses > 0)
                    std::cout << "Win Rate:          " << std::setprecision(1)
                              << (100.0 * wins / (wins + losses)) << "%\n";
                std::cout << "Gross Profit:      $" << std::setprecision(2) << gross_profit << "\n";
                std::cout << "Gross Loss:        $" << gross_loss << "\n";
                std::cout << "Total Commissions: $" << total_commission << "\n";
                std::cout << "============================\n";
            }
        }
    }

    // ========================================================================
    // IEventHandler Implementation (called by EventLoop on its thread)
    // ========================================================================

    void on_depth_event(const DepthEvent& event) override {
        // Reconstruct a DepthUpdate from compact event
        exchange::binance::DepthUpdate update;
        update.symbol = Symbol(std::string_view(event.symbol, strnlen(event.symbol, 16)));
        update.last_update_id = event.sequence;
        update.event_time = from_epoch_ms(event.timestamp_ms);

        update.bids.resize(event.bid_count);
        for (uint16_t i = 0; i < event.bid_count; ++i) {
            update.bids[i].price = Price(event.bids[i].price_raw);
            update.bids[i].quantity = Quantity(event.bids[i].quantity_raw);
            update.bids[i].order_count = 0;
        }
        update.asks.resize(event.ask_count);
        for (uint16_t i = 0; i < event.ask_count; ++i) {
            update.asks[i].price = Price(event.asks[i].price_raw);
            update.asks[i].quantity = Quantity(event.asks[i].quantity_raw);
            update.asks[i].order_count = 0;
        }

        handle_depth_update(update);

        // Forward raw DepthEvent to ArbitrageEngine (zero-copy, same thread)
        if (arb_engine_) {
            arb_engine_->on_depth_event(event);
        }
    }

    void on_timer_event(const TimerEvent& event) override {
        if (event.timer_id == timer_id::STATS) {
            print_stats();
            if (arb_engine_) arb_engine_->print_stats();
            if (arb_exec_) arb_exec_->sync_fills();
        }
        else if (event.timer_id == timer_id::POSITION_SYNC) {
            if (risk_manager_->open_positions() > 0 || position_tracker_->has_open_position()) {
                if (position_tracker_->sync_with_exchange()) {
                    risk_manager_->on_position_closed(0.0);
                    std::cout << "[TRACKER] Position Closed (Sync).\n";
                }
            }
        }
        else if (event.timer_id == timer_id::HEARTBEAT) {
            // Periodic Telegram heartbeat (every 5 min)
            if (telegram_) {
                double balance = 0.0;
                int open_pos = static_cast<int>(risk_manager_->open_positions());
                double daily_pnl = 0.0;
                auto acc = client_->get_account_info();
                if (acc) {
                    balance = acc->available_balance;
                    daily_pnl = acc->total_unrealized_profit;
                }
                // telegram_->notify_heartbeat(balance, open_pos, daily_pnl);
            }
        }
    }

    void on_shutdown_event(const ShutdownEvent&) override {
        std::cout << "[EVENT] Shutdown event received\n";
    }
    
private:
    // ========================================================================
    // Core Strategy Pipeline (shared by live & replay paths)
    // ========================================================================


    void handle_depth_update(const exchange::binance::DepthUpdate& update) {
        // STRICT CHECK: Only process updates for the main trading symbol
        if (update.symbol.view() != config_.symbol) {
             return; 
        }

        last_update_time_ = std::chrono::steady_clock::now();
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
        
        stats_.total_updates++;
        stats_.last_latency_us = latency;
        stats_.avg_latency_us = (stats_.avg_latency_us * 0.99) + (latency * 0.01);
        
        // Check for trading signal
        if (obi_generator_->is_ready()) {
            double imbalance = obi_generator_->smoothed_imbalance();
            
            const auto* best_bid = order_book_.best_bid();
            const auto* best_ask = order_book_.best_ask();
            double spread_pct = 0.0;
            if (best_bid && best_ask && best_bid->price.to_double() > 0) {
                spread_pct = (best_ask->price.to_double() - best_bid->price.to_double()) 
                           / best_bid->price.to_double() * 100.0;
            }
            
            if (std::abs(imbalance) > config_.imbalance_threshold) {
                if (imbalance > 0) stats_.buy_signals++;
                else stats_.sell_signals++;
            }
            
            auto filtered = signal_filter_->filter(
                imbalance, spread_pct,
                best_bid ? best_bid->price.to_double() : 0.0,
                best_ask ? best_ask->price.to_double() : 0.0
            );
            
            if (filtered.type != strategy::SignalFilter::SignalType::None) {
                bool is_long = (filtered.type == strategy::SignalFilter::SignalType::Buy);
                double price = is_long ? (best_ask ? best_ask->price.to_double() : 0) 
                                     : (best_bid ? best_bid->price.to_double() : 0);
                
                if (is_long) stats_.qualified_buys++;
                else stats_.qualified_sells++;

                if (!config_.trading_enabled) {
                    stats_.rejected_trades++;
                    if (!trading_disabled_notified_) {
                        std::cout << "[WATCH] Trading disabled; signals will be ignored.\n";
                        if (telegram_) {
                            telegram_->send("🟡 *WATCH ONLY*\nTrading disabled; signals ignored.",
                                             network::TelegramPriority::WARNING,
                                             "watch_only");
                        }
                        trading_disabled_notified_ = true;
                    }
                    return;
                }
                
                if (price <= 0.0) {
                     std::cerr << "[EXEC] Invalid price (" << price << "), skipping trade.\n";
                     return;
                }

                auto decision = risk_manager_->can_trade(price, is_long);
                
                if (decision.decision == risk::RiskManager::TradeDecision::Approved) {
                    stats_.approved_trades++;
                    
                    std::cout << "\n[EXEC] Placing Bracket Order..."
                              << " | Price: " << price
                              << " | Size: $" << decision.position_size_usd
                              << "\n";

                    Price sl_price = Price::from_double(round_price(decision.stop_loss_price));
                    Price tp_price = Price::from_double(round_price(decision.take_profit_price));

                    constexpr double min_notional_usd = 100.0;
                    double raw_qty = decision.position_size_usd / price;
                    double rounded_qty = round_quantity_for_obi(raw_qty, price);
                    double notional = rounded_qty * price;

                    // SAFETY GUARD: Prevent massive orders (e.g. due to bad price)
                    if (notional > 600.0) {
                        std::cerr << "[CRITICAL] Order rejected: Notional $" << notional 
                                  << " exceeds safety limit ($600). Price=" << price 
                                  << " Qty=" << rounded_qty << "\n";
                        if (telegram_) telegram_->notify_error("CRITICAL: Safety limit triggered! Notional > $600");
                        return;
                    }

                    if (notional < min_notional_usd) {
                        double step;
                        if (price >= 10000.0)      step = 0.001;
                        else if (price >= 1000.0)  step = 0.01;
                        else if (price >= 100.0)   step = 0.1;
                        else if (price >= 10.0)    step = 1.0;
                        else                        step = 10.0;

                        double min_qty = std::ceil((min_notional_usd / price) / step) * step;
                        rounded_qty = min_qty;
                        notional = rounded_qty * price;
                        std::cout << "[RISK] Bumped size to meet min notional: $"
                                  << std::fixed << std::setprecision(2) << notional << "\n";
                    }

                    std::cout << "[EXEC] Qty: " << std::fixed << std::setprecision(6)
                              << rounded_qty << " | Notional: $"
                              << std::setprecision(2) << notional << "\n";

                    if (rounded_qty <= 0.0) {
                        stats_.rejected_trades++;
                        std::cout << "\n[TRADE REJECTED] Reason: qty rounded to 0 (price tier step)\n";
                        return;
                    }

                    auto result = order_manager_->place_bracket_order(
                        Symbol(config_.symbol),
                        is_long ? Side::Buy : Side::Sell,
                        Quantity::from_double(rounded_qty),
                        sl_price,
                        tp_price
                    );

                    if (result.entry_order) {
                        risk_manager_->on_order_placed(); 
                        std::cout << "[EXEC] Entry Filled: " << result.entry_order->price.to_double() << "\n";
                        if (result.stop_loss_order) std::cout << "[EXEC] SL Placed: " << decision.stop_loss_price << "\n";
                        else std::cerr << "[EXEC] SL Failed!\n";
                        if (result.take_profit_order) std::cout << "[EXEC] TP Placed: " << decision.take_profit_price << "\n";
                        else std::cerr << "[EXEC] TP Failed!\n";

                        if (!result.stop_loss_order || !result.take_profit_order) {
                            std::cerr << "[EXEC] SL/TP placement failed. Emergency close.\n";
                            auto close_side = is_long ? Side::Sell : Side::Buy;
                            Quantity close_qty = result.entry_order->executed_qty.is_valid()
                                                     ? result.entry_order->executed_qty
                                                     : Quantity::from_double(rounded_qty);
                            order_manager_->place_reduce_only_market_order(
                                Symbol(config_.symbol), close_side, close_qty);
                            if (telegram_) {
                                telegram_->notify_error("SL/TP placement failed; emergency close sent");
                            }
                            return;
                        }

                        // Telegram: OBI trade notification
                        if (telegram_) {
                            telegram_->notify_obi_trade(
                                config_.symbol, is_long, price,
                                decision.position_size_usd,
                                decision.stop_loss_price,
                                decision.take_profit_price);
                        }
                    } else {
                        std::cerr << "[EXEC] Entry Order Failed!\n";
                        if (telegram_) telegram_->notify_error("OBI Entry Order Failed!");
                    }
                } else {
                    stats_.rejected_trades++;
                    std::cout << "\n[TRADE REJECTED] Reason: " << decision.reason << "\n";
                }
            }
        }
        
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
                  << "Lat: " << std::setprecision(0) << stats_.avg_latency_us << "us";

        // Stale data warning
        auto since_last = std::chrono::steady_clock::now() - last_update_time_;
        auto stale_secs = std::chrono::duration_cast<std::chrono::seconds>(since_last).count();
        if (stale_secs > 30) {
            std::cout << " | [STALE " << stale_secs << "s]";
        }

        // Show ring buffer stats in live mode
        if (message_bus_) {
            std::cout << " | Q: " << message_bus_->queue_size()
                      << " | Drop: " << message_bus_->events_dropped();
        }
        std::cout << "\n";

        // Arbitrage spread summary
        if (arb_engine_) {
            for (const auto& key : arb_engine_->pair_keys()) {
                const auto& snap = arb_engine_->last_spread(key);
                const auto& pos = arb_engine_->position(key);
                bool exec_open = arb_exec_ && arb_exec_->has_open_position(key);
                std::cout << "  [ARB] " << key
                          << " | Z: " << std::setprecision(3)
                          << std::showpos << snap.z_score << std::noshowpos
                          << " | Corr: " << std::setprecision(3) << snap.correlation
                          << " | " << (pos.is_flat() ? "FLAT" : "OPEN")
                          << (exec_open ? " [EXEC]" : "")
                          << "\n";
            }
            if (arb_exec_) {
                const auto& es = arb_exec_->stats();
                std::cout << "  [EXEC] Signals:" << es.total_signals
                          << " Filled:" << es.both_filled
                          << " PnL:" << std::setprecision(3) << std::showpos
                          << es.realized_pnl << "%" << std::noshowpos << "\n";
            }
        }
    }
    
    // ========================================================================
    // Members
    // ========================================================================

    // Config
    AppConfig config_;

    // Telegram Notifier
    std::unique_ptr<network::TelegramNotifier> telegram_;

    // Exchange
    std::unique_ptr<exchange::binance::IBinanceClient> client_;
    exchange::MockClient* mock_client_ptr_ = nullptr;

    // Event-Driven Infrastructure (live mode only)
    std::unique_ptr<EventLoop> event_loop_;
    std::unique_ptr<MessageBus> message_bus_;

    // Replay Infrastructure
    std::unique_ptr<market::ReplayEngine> replay_engine_;

    // Multi-Symbol Replay (arb-backtest mode)
    std::unique_ptr<market::MultiSymbolReplay> multi_replay_;
    std::optional<market::SyntheticPairConfig> synthetic_pair_config_;

    // Arbitrage Engine
    std::unique_ptr<strategy::arbitrage::ArbitrageEngine> arb_engine_;

    // Arbitrage Execution Manager
    std::unique_ptr<strategy::arbitrage::ArbExecutionManager> arb_exec_;

    // Strategy Components
    market::OrderBook order_book_;
    std::unique_ptr<strategy::ImbalanceSignalGenerator> obi_generator_;
    std::unique_ptr<strategy::SignalFilter> signal_filter_;
    std::unique_ptr<risk::RiskManager> risk_manager_;
    std::unique_ptr<order::OrderManager> order_manager_;
    std::unique_ptr<order::PositionTracker> position_tracker_;
    std::unique_ptr<market::DataRecorder> recorder_;
    
    // State
    double last_imbalance_ = 0.0;
    bool trading_disabled_notified_ = false;
    std::chrono::steady_clock::time_point last_update_time_ = std::chrono::steady_clock::now();
    
    // Statistics
    struct Stats {
        uint64_t total_updates = 0;
        uint64_t buy_signals = 0;
        uint64_t sell_signals = 0;
        uint64_t qualified_buys = 0;
        uint64_t qualified_sells = 0;
        uint64_t approved_trades = 0;
        uint64_t rejected_trades = 0;
        int64_t last_latency_us = 0;
        double avg_latency_us = 0.0;
    } stats_;
};

// ============================================================================
// Main Entry Point
// ============================================================================

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    std::string config_path = "config/config.yaml";
    if (argc > 1 && argv[1][0] != '-') {
        config_path = argv[1];
    }
    
    std::cout << "[INFO] Loading config from: " << config_path << "\n";
    auto config = load_config(config_path);
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            std::string mode = argv[++i];
            if (mode == "replay") config.replay_mode = true;
        }
        else if (arg == "--file" && i + 1 < argc) {
            config.replay_file = argv[++i];
        }
        else if (arg == "--speed" && i + 1 < argc) {
            config.replay_speed = std::stod(argv[++i]);
        }
        else if (arg == "--synthetic-ticks" && i + 1 < argc) {
            config.synthetic_ticks = static_cast<size_t>(std::stoi(argv[++i]));
        }
        else if (arg == "--arb-file" && i + 1 < argc) {
            config.arb_replay_files.push_back(argv[++i]);
        }
    }

    // Detect arb-backtest from --mode
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            if (std::string(argv[i + 1]) == "arb-backtest") {
                config.arb_backtest_mode = true;
                config.arbitrage_enabled = true;  // Force enable
            }
        }
    }
    
    if (!config.replay_mode && !config.arb_backtest_mode) {
        if (config.api_key.empty() || config.api_key.find("BURAYA") != std::string::npos) {
            std::cerr << "\n[ERROR] API key not configured!\n";
            std::cerr << "[HINT] Edit config/config.yaml and add your Testnet API keys\n";
            std::cerr << "       Get keys from: https://testnet.binancefuture.com\n\n";
            return 1;
        }
    } else {
        std::cout << ">>> STARTING IN REPLAY MODE <<<\n";
    }

    if (config.arb_backtest_mode) {
        std::cout << ">>> STARTING IN ARB-BACKTEST MODE <<<\n";
        if (config.arb_replay_files.empty()) {
            std::cout << ">>> SYNTHETIC DATA: " << config.synthetic_ticks << " ticks <<<\n";
        } else {
            std::cout << ">>> FILES: " << config.arb_replay_files.size() << " <<<\n";
        }
    }
    
    OBITradingEngine engine(config);
    
    // Wire SIGINT to EventLoop shutdown
    static OBITradingEngine* g_engine = &engine;
    std::signal(SIGINT, [](int sig) {
        std::cout << "\n[SIGNAL] Received " << sig << ", stopping...\n";
        g_running = false;
        // If live mode, also stop the EventLoop reactor
        if (g_engine) g_engine->stop();
    });
    
    try {
        if (!engine.start()) {
        return 1;
    }
    
    engine.run();
    engine.stop();
    
    } catch (const std::exception& e) {
        std::cerr << "\n[CRITICAL] Unhandled Exception: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "\n[CRITICAL] Unknown Exception!\n";
        return 1;
    }
    
    return 0;
}
