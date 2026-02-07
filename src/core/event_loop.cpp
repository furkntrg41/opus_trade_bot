// ============================================================================
// OPUS TRADE BOT - Event Loop Implementation
// ============================================================================

#include "opus/core/types.hpp"

// TODO: Implement event loop using Boost.Asio
// This will be the central I/O event dispatcher

namespace opus {

class EventLoop {
public:
    EventLoop() = default;
    ~EventLoop() = default;

    void run() {
        // TODO: io_context.run()
    }

    void stop() {
        // TODO: io_context.stop()
    }

private:
    // boost::asio::io_context io_context_;
};

}  // namespace opus
