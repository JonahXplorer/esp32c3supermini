#pragma once

#include <stdint.h>

namespace agentlink::wifi {

enum class CloudRecovery { kNone, kReauthenticate, kRejoinStation };

// Count separate DNS failures, not several clients receiving the same failure.
// Call only under the WiFi mutex. Monotonic times are in milliseconds.
class DnsRecovery {
public:
    void Resolved() {
        first_failure_ms_ = -1;
        pending_ = false;
    }

    void Failed(int64_t now) {
        if (pending_) return;
        if (first_failure_ms_ < 0 || now - first_failure_ms_ > 30000) {
            first_failure_ms_ = now;
        } else if (now - first_failure_ms_ >= 1500) {
            pending_ = true;
        }
    }

    CloudRecovery Take(int64_t now, bool cloud_online) {
        if (!pending_) return CloudRecovery::kNone;
        if (now >= rejoin_after_ms_) {
            rejoin_after_ms_ = now + 60000;
            Resolved();
            return CloudRecovery::kRejoinStation;
        }
        if (cloud_online) {
            Resolved();
            return CloudRecovery::kReauthenticate;
        }
        return CloudRecovery::kNone;
    }

private:
    int64_t first_failure_ms_ = -1;
    int64_t rejoin_after_ms_ = 0;
    bool pending_ = false;
};

}  // namespace agentlink::wifi

// HTTP workers snapshot this generation before opening or reusing a socket.
uint32_t wifi_cloud_network_generation();
void wifi_cloud_dns_result(uint32_t generation, bool resolved);
bool wifi_cloud_requests_allowed();
