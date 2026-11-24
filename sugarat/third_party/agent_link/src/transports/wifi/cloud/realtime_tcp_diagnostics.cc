#include "realtime_tcp_diagnostics.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/api.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/priv/tcpip_priv.h"

extern "C" {
#include "lwip/priv/sockets_priv.h"
}

namespace agentlink::realtime {
namespace {

constexpr const char* kTag = "agent_link.realtime";

struct TcpSnapshot : tcpip_api_call_data {
    int fd;
    uint32_t ack;
    uint32_t snd_next;
    unsigned peer_window;
    unsigned congestion_window;
    unsigned send_free;
    unsigned queue_pbufs;
    unsigned unsent;
    unsigned unacked;
    unsigned retransmits;
    unsigned duplicate_acks;
    unsigned persist_probes;
    int rto_ms;
    unsigned state;
};

unsigned SegmentBytes(const tcp_seg* segment) {
    unsigned bytes = 0;
    for (; segment; segment = segment->next) bytes += segment->len;
    return bytes;
}

err_t ReadTcpSnapshot(tcpip_api_call_data* request) {
    auto& sample = *static_cast<TcpSnapshot*>(request);
    // ESP-IDF lwIP internals. Read only in the TCP/IP core and never retain a PCB pointer.
    const lwip_sock* sock = lwip_socket_dbg_get_socket(sample.fd);
    if (!sock || !sock->conn || NETCONNTYPE_GROUP(sock->conn->type) != NETCONN_TCP)
        return ERR_CONN;
    const tcp_pcb* pcb = sock->conn->pcb.tcp;
    if (!pcb) return ERR_CONN;

    sample.ack = pcb->lastack;
    sample.snd_next = pcb->snd_nxt;
    sample.peer_window = pcb->snd_wnd;
    sample.congestion_window = pcb->cwnd;
    sample.send_free = pcb->snd_buf;
    sample.queue_pbufs = pcb->snd_queuelen;
    sample.unsent = SegmentBytes(pcb->unsent);
    sample.unacked = SegmentBytes(pcb->unacked);
    sample.retransmits = pcb->nrtx;
    sample.duplicate_acks = pcb->dupacks;
    sample.persist_probes = pcb->persist_probe;
    sample.rto_ms = pcb->rto * TCP_SLOW_INTERVAL;
    sample.state = pcb->state;
    return ERR_OK;
}

}  // namespace

void LogTcpStall(esp_tls_t* tls, TcpStallPhase phase, TcpStallStart& start) {
    const bool at_start = phase == TcpStallPhase::kStart;
    const char* label = at_start ? "start" : "timeout";
    if (at_start) start = {};

    TcpSnapshot sample{};
    const int64_t requested_us = esp_timer_get_time();
    const bool have_fd = esp_tls_get_conn_sockfd(tls, &sample.fd) == ESP_OK && sample.fd >= 0;
    const err_t err = have_fd ? tcpip_api_call(&ReadTcpSnapshot, &sample) : static_cast<err_t>(ERR_CONN);
    const auto sample_ms = static_cast<unsigned>((esp_timer_get_time() - requested_us) / 1000);
    if (err != ERR_OK) {
        ESP_LOGW(kTag, "Voice TCP write stall: phase=%s sample=unavailable lwip_err=%d sample_ms=%u",
                 label, static_cast<int>(err), sample_ms);
        return;
    }

    if (at_start) start = {sample.ack, true};
    const int64_t acked_bytes = start.sampled ? static_cast<uint32_t>(sample.ack - start.ack) : -1LL;
    // lwIP's retransmit count (nrtx) is reset when the cumulative ACK advances.
    ESP_LOGI(kTag, "Voice TCP write stall: phase=%s fd=%d state=%u ack=%lu acked_bytes=%lld snd_next=%lu "
                   "retries_since_ack=%u dup_acks=%u rto_ms=%d peer_wnd=%u cwnd=%u zero_wnd_probes=%u "
                   "snd_free=%u/%u snd_pbufs=%u/%u unsent=%u unacked=%u sample_ms=%u",
             label, sample.fd, sample.state, static_cast<unsigned long>(sample.ack),
             static_cast<long long>(acked_bytes), static_cast<unsigned long>(sample.snd_next),
             sample.retransmits, sample.duplicate_acks, sample.rto_ms,
             sample.peer_window, sample.congestion_window, sample.persist_probes,
             sample.send_free, static_cast<unsigned>(TCP_SND_BUF),
             sample.queue_pbufs, static_cast<unsigned>(TCP_SND_QUEUELEN),
             sample.unsent, sample.unacked, sample_ms);
}

}  // namespace agentlink::realtime
