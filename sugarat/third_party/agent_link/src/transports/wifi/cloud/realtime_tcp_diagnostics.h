#pragma once

#include <cstdint>

#include "esp_tls.h"

namespace agentlink::realtime {

// One realtime WSS worker owns the trace for the lifetime of its socket.
class TcpWifiTrace {
public:
    explicit TcpWifiTrace(esp_tls_t* tls);
    ~TcpWifiTrace();
    TcpWifiTrace(const TcpWifiTrace&) = delete;
    TcpWifiTrace& operator=(const TcpWifiTrace&) = delete;
private:
    bool armed_ = false;
};

struct TcpStallStart {
    uint32_t ack = 0;
    uint32_t sequence = 0;
    uint32_t end = 0;
    bool sampled = false;
    bool has_segment = false;
};

enum class TcpStallPhase { kStart, kDuring, kResume, kTimeout };

struct TcpReadGapStart {
    uint32_t next = 0;
    bool sampled = false;
};

enum class TcpReadGapPhase { kStart, kResume, kClosed };

// The WSS worker must own the socket for the duration of the snapshot.
void LogTcpStall(esp_tls_t* tls, TcpStallPhase phase, TcpStallStart& start);
void LogTcpReadGap(esp_tls_t* tls, TcpReadGapPhase phase, TcpReadGapStart& start, unsigned gap_ms);

}  // namespace agentlink::realtime
