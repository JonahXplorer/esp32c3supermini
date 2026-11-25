#pragma once

#include <cstdint>

#include "esp_tls.h"

namespace agentlink::realtime {

struct TcpStallStart {
    uint32_t ack = 0;
    bool sampled = false;
};

enum class TcpStallPhase { kStart, kTimeout };

struct TcpReadGapStart {
    uint32_t next = 0;
    bool sampled = false;
};

enum class TcpReadGapPhase { kStart, kResume, kClosed };

// The WSS worker must own the socket for the duration of the snapshot.
void LogTcpStall(esp_tls_t* tls, TcpStallPhase phase, TcpStallStart& start);
void LogTcpReadGap(esp_tls_t* tls, TcpReadGapPhase phase, TcpReadGapStart& start, unsigned gap_ms);

}  // namespace agentlink::realtime
