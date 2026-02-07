// ============================================================================
// OPUS TRADE BOT - Risk Manager Implementation
// ============================================================================
// Position sizing, stop-loss/take-profit, and drawdown protection
// Note: Core risk management is header-only in risk_manager.hpp
// This file provides additional utilities
// ============================================================================

#include "opus/order/risk_manager.hpp"

namespace opus::order {

// Risk manager core implementation is header-only
// This file provides a convenient factory and configuration loader

// ============================================================================
// Risk Manager Factory
// ============================================================================

std::unique_ptr<RiskManager> create_risk_manager(const RiskConfig& config) {
    return std::make_unique<RiskManager>(config);
}

// ============================================================================
// Default Conservative Configuration
// ============================================================================

RiskConfig create_conservative_config() {
    RiskConfig config;
    config.risk_per_trade_pct = 0.01;       // 1% per trade
    config.max_position_pct = 0.05;          // 5% max position
    config.max_total_exposure_pct = 0.25;    // 25% total exposure
    config.default_stop_loss_pct = 0.01;     // 1% stop loss
    config.default_take_profit_pct = 0.02;   // 2% take profit
    config.max_daily_drawdown_pct = 0.03;    // 3% daily max loss
    config.max_leverage = 5;
    return config;
}

// ============================================================================
// Default Aggressive Configuration
// ============================================================================

RiskConfig create_aggressive_config() {
    RiskConfig config;
    config.risk_per_trade_pct = 0.03;        // 3% per trade
    config.max_position_pct = 0.15;          // 15% max position
    config.max_total_exposure_pct = 0.60;    // 60% total exposure
    config.default_stop_loss_pct = 0.025;    // 2.5% stop loss
    config.default_take_profit_pct = 0.05;   // 5% take profit
    config.max_daily_drawdown_pct = 0.08;    // 8% daily max loss
    config.max_leverage = 20;
    return config;
}

// ============================================================================
// Scalping Configuration
// ============================================================================

RiskConfig create_scalping_config() {
    RiskConfig config;
    config.risk_per_trade_pct = 0.005;       // 0.5% per trade
    config.max_position_pct = 0.10;          // 10% max position
    config.max_total_exposure_pct = 0.30;    // 30% total exposure
    config.default_stop_loss_pct = 0.003;    // 0.3% tight stop loss
    config.default_take_profit_pct = 0.005;  // 0.5% take profit
    config.use_trailing_stop = true;
    config.trailing_stop_pct = 0.002;        // 0.2% trailing
    config.max_daily_drawdown_pct = 0.02;    // 2% daily max loss
    config.max_orders_per_minute = 120;      // High frequency
    config.max_leverage = 10;
    return config;
}

}  // namespace opus::order
