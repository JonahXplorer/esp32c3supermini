#include "realtime_tcp_diagnostics.h"

#include <atomic>
#include <cstring>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lwip/api.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/priv/tcpip_priv.h"

extern "C" {
#include "lwip/priv/sockets_priv.h"
}

namespace agentlink::realtime {
namespace {

constexpr const char* kTag = "agent_link.realtime";
constexpr unsigned kTxHistorySize = 128;

struct TcpHeader {
    const uint8_t* source;
    const uint8_t* destination;
    unsigned ip_bytes;
    unsigned source_port;
    unsigned destination_port;
    uint32_t seq;
    uint32_t ack;
    unsigned data_bytes;
    bool has_ack;
};

struct TraceFlow : tcpip_api_call_data {
    int fd;
    unsigned ip_bytes;
    unsigned local_port;
    unsigned remote_port;
    uint8_t local_ip[16];
    uint8_t remote_ip[16];
};

struct TxEvent {
    uint32_t seq;
    uint32_t end;
    uint32_t ms;
    esp_err_t result;
    uint16_t ref_before;
    uint16_t ref_after;
};

struct AckProgress {
    unsigned count;
    uint32_t highest;
    uint32_t highest_ms;
};

struct AckTrace {
    AckProgress ingress;
    AckProgress queued;
    esp_err_t last_result;
};

std::atomic_bool s_trace_active{false};
portMUX_TYPE s_trace_lock = portMUX_INITIALIZER_UNLOCKED;
TraceFlow s_flow{};
uint32_t s_generation = 0;
AckTrace s_ack{};

// lwIP calls the Wi-Fi transmit wrapper and reads this history on its TCP/IP thread.
EXT_RAM_BSS_ATTR static TxEvent s_tx_history[kTxHistorySize];
unsigned s_tx_cursor = 0;
unsigned s_tx_count = 0;

unsigned Read16(const uint8_t* p) {
    return (static_cast<unsigned>(p[0]) << 8) | p[1];
}

uint32_t Read32(const uint8_t* p) {
    return (static_cast<uint32_t>(Read16(p)) << 16) | Read16(p + 2);
}

// The netif interfaces carry Ethernet frames. Only inspect IP and TCP headers.
bool ReadTcpHeader(const void* buffer, size_t bytes, TcpHeader& packet) {
    if (!buffer || bytes < 14) return false;
    const auto* frame = static_cast<const uint8_t*>(buffer);
    unsigned type = Read16(frame + 12);
    size_t offset = 14;
    if (type == 0x8100 && bytes >= 18) {
        type = Read16(frame + 16);
        offset = 18;
    }
    const uint8_t* ip = frame + offset;
    bytes -= offset;
    unsigned ip_size;
    if (type == 0x0800 && bytes >= 20 && (ip[0] >> 4) == 4) {
        offset = (ip[0] & 0x0f) * 4;
        if (offset < 20 || ip[9] != 6 || (Read16(ip + 6) & 0x3fff)) return false;
        ip_size = Read16(ip + 2);
        packet.source = ip + 12;
        packet.destination = ip + 16;
        packet.ip_bytes = 4;
    } else if (type == 0x86dd && bytes >= 40 && (ip[0] >> 4) == 6) {
        ip_size = 40 + Read16(ip + 4);
        if (ip_size > bytes) return false;
        offset = 40;
        unsigned protocol = ip[6];
        while (protocol == 0 || protocol == 43 || protocol == 60) {
            if (offset + 2 > ip_size) return false;
            protocol = ip[offset];
            offset += (ip[offset + 1] + 1) * 8;
        }
        if (protocol != 6) return false;
        packet.source = ip + 8;
        packet.destination = ip + 24;
        packet.ip_bytes = 16;
    } else {
        return false;
    }
    if (ip_size > bytes || offset + 20 > ip_size) return false;
    const uint8_t* tcp = ip + offset;
    const unsigned tcp_size = (tcp[12] >> 4) * 4;
    if (tcp_size < 20 || offset + tcp_size > ip_size) return false;
    packet.source_port = Read16(tcp);
    packet.destination_port = Read16(tcp + 2);
    packet.seq = Read32(tcp + 4);
    packet.ack = Read32(tcp + 8);
    packet.data_bytes = ip_size - offset - tcp_size;
    packet.has_ack = (tcp[13] & TCP_ACK) != 0;
    return true;
}

uint32_t MatchTrace(const TcpHeader& packet, bool ingress) {
    const unsigned local_port = ingress ? packet.destination_port : packet.source_port;
    const unsigned remote_port = ingress ? packet.source_port : packet.destination_port;
    const uint8_t* local_ip = ingress ? packet.destination : packet.source;
    const uint8_t* remote_ip = ingress ? packet.source : packet.destination;
    portENTER_CRITICAL(&s_trace_lock);
    const bool matches = s_trace_active.load(std::memory_order_relaxed) &&
                         packet.ip_bytes == s_flow.ip_bytes && local_port == s_flow.local_port &&
                         remote_port == s_flow.remote_port &&
                         memcmp(local_ip, s_flow.local_ip, packet.ip_bytes) == 0 &&
                         memcmp(remote_ip, s_flow.remote_ip, packet.ip_bytes) == 0;
    const uint32_t generation = matches ? s_generation : 0;
    portEXIT_CRITICAL(&s_trace_lock);
    return generation;
}

void NoteAck(AckProgress& progress, uint32_t ack, uint32_t ms) {
    if (!progress.count || TCP_SEQ_LT(progress.highest, ack)) {
        progress.highest = ack;
        progress.highest_ms = ms;
    }
    ++progress.count;
}

uint16_t BufferRef(const pbuf* buffer) {
    if (!buffer) return 0;
    SYS_ARCH_DECL_PROTECT(level);
    SYS_ARCH_PROTECT(level);
    const uint16_t ref = buffer->ref;
    SYS_ARCH_UNPROTECT(level);
    return ref;
}

const tcp_pcb* SocketPcb(int fd) {
    const lwip_sock* sock = lwip_socket_dbg_get_socket(fd);
    if (!sock || !sock->conn || NETCONNTYPE_GROUP(sock->conn->type) != NETCONN_TCP) return nullptr;
    return sock->conn->pcb.tcp;
}

err_t ArmTrace(tcpip_api_call_data* request) {
    auto& flow = *static_cast<TraceFlow*>(request);
    const tcp_pcb* pcb = SocketPcb(flow.fd);
    if (!pcb) return ERR_CONN;

    flow.local_port = pcb->local_port;
    flow.remote_port = pcb->remote_port;
#if LWIP_IPV6
    if (IP_IS_V6(&pcb->remote_ip)) {
        flow.ip_bytes = 16;
        memcpy(flow.local_ip, ip_2_ip6(&pcb->local_ip)->addr, flow.ip_bytes);
        memcpy(flow.remote_ip, ip_2_ip6(&pcb->remote_ip)->addr, flow.ip_bytes);
    } else
#endif
    {
        flow.ip_bytes = 4;
        const uint32_t local = ip4_addr_get_u32(ip_2_ip4(&pcb->local_ip));
        const uint32_t remote = ip4_addr_get_u32(ip_2_ip4(&pcb->remote_ip));
        memcpy(flow.local_ip, &local, flow.ip_bytes);
        memcpy(flow.remote_ip, &remote, flow.ip_bytes);
    }

    s_tx_count = s_tx_cursor = 0;
    portENTER_CRITICAL(&s_trace_lock);
    s_flow = flow;
    if (++s_generation == 0) ++s_generation;
    s_ack = {};
    s_trace_active.store(true, std::memory_order_release);
    portEXIT_CRITICAL(&s_trace_lock);
    return ERR_OK;
}

struct TcpSnapshot : tcpip_api_call_data {
    int fd;
    TcpStallStart* stall;
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
    uint32_t recv_next;
    unsigned recv_window;
    unsigned advertised_window;
    unsigned out_of_order;
    uint32_t ticks;
    int rtime_ms;
    int watched_ref;
    unsigned unacked_busy;
    bool watched_unsent;
    bool link_traced;
    unsigned tx_attempts;
    unsigned tx_ok;
    unsigned tx_history_count;
    TxEvent tx_last;
    AckTrace rx;
};

unsigned SegmentBytes(const tcp_seg* segment) {
    unsigned bytes = 0;
    for (; segment; segment = segment->next) bytes += segment->len;
    return bytes;
}

bool CoversSequence(uint32_t begin, uint32_t end, uint32_t sequence) {
    return TCP_SEQ_LEQ(begin, sequence) && TCP_SEQ_LT(sequence, end);
}

void ReadStallSnapshot(const tcp_pcb& pcb, TcpSnapshot& sample) {
    auto& stall = *sample.stall;
    sample.ticks = tcp_ticks;
    sample.rtime_ms = pcb.rtime < 0 ? -1 : pcb.rtime * TCP_SLOW_INTERVAL;
    sample.watched_ref = -1;
    if (!stall.has_segment) {
        const tcp_seg* oldest = pcb.unacked ? pcb.unacked : pcb.unsent;
        if (oldest && oldest->len) {
            stall.sequence = lwip_ntohl(oldest->tcphdr->seqno);
            stall.end = stall.sequence + oldest->len;
            stall.has_segment = true;
        }
    }
    for (const tcp_seg* seg = pcb.unacked; seg; seg = seg->next) {
        const uint16_t ref = BufferRef(seg->p);
        if (ref != 1) ++sample.unacked_busy;
        const uint32_t seq = lwip_ntohl(seg->tcphdr->seqno);
        if (stall.has_segment && CoversSequence(seq, seq + seg->len, stall.sequence))
            sample.watched_ref = ref;
    }
    for (const tcp_seg* seg = pcb.unsent; stall.has_segment && sample.watched_ref < 0 && seg; seg = seg->next) {
        const uint32_t seq = lwip_ntohl(seg->tcphdr->seqno);
        if (CoversSequence(seq, seq + seg->len, stall.sequence)) {
            sample.watched_unsent = true;
            sample.watched_ref = BufferRef(seg->p);
        }
    }

    portENTER_CRITICAL(&s_trace_lock);
    sample.link_traced = s_trace_active.load(std::memory_order_relaxed) && s_flow.fd == sample.fd;
    if (sample.link_traced) sample.rx = s_ack;
    portEXIT_CRITICAL(&s_trace_lock);
    if (!sample.link_traced || !stall.has_segment) return;

    sample.tx_history_count = s_tx_count;
    for (unsigned i = 0; i < s_tx_count; ++i) {
        const TxEvent& event = s_tx_history[(s_tx_cursor + kTxHistorySize - 1 - i) % kTxHistorySize];
        if (!CoversSequence(event.seq, event.end, stall.sequence)) continue;
        if (!sample.tx_attempts) sample.tx_last = event;
        ++sample.tx_attempts;
        if (event.result == ESP_OK) ++sample.tx_ok;
    }
}

err_t ReadTcpSnapshot(tcpip_api_call_data* request) {
    auto& sample = *static_cast<TcpSnapshot*>(request);
    // ESP-IDF lwIP internals. Read only in the TCP/IP core and never retain a PCB pointer.
    const tcp_pcb* pcb = SocketPcb(sample.fd);
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
    sample.recv_next = pcb->rcv_nxt;
    sample.recv_window = pcb->rcv_wnd;
    sample.advertised_window = pcb->rcv_ann_wnd;
#if TCP_QUEUE_OOSEQ
    sample.out_of_order = SegmentBytes(pcb->ooseq);
#endif
    if (sample.stall) ReadStallSnapshot(*pcb, sample);
    return ERR_OK;
}

err_t SampleTcp(esp_tls_t* tls, TcpSnapshot& sample) {
    if (esp_tls_get_conn_sockfd(tls, &sample.fd) != ESP_OK || sample.fd < 0) return ERR_CONN;
    return tcpip_api_call(&ReadTcpSnapshot, &sample);
}

}  // namespace

extern "C" esp_err_t __real_esp_netif_transmit_wrap(esp_netif_t*, void*, size_t, void*);
extern "C" esp_err_t __real_esp_netif_receive(esp_netif_t*, void*, size_t, void*);

extern "C" esp_err_t __wrap_esp_netif_transmit_wrap(esp_netif_t* netif, void* data, size_t len, void* netstack_buf) {
    TcpHeader packet{};
    const bool trace = s_trace_active.load(std::memory_order_acquire) && ReadTcpHeader(data, len, packet) &&
                       packet.data_bytes && MatchTrace(packet, false);
    const auto* buffer = static_cast<const pbuf*>(netstack_buf);
    const uint16_t ref_before = trace ? BufferRef(buffer) : 0;
    const esp_err_t result = __real_esp_netif_transmit_wrap(netif, data, len, netstack_buf);
    if (trace) {
        const uint16_t ref_after = BufferRef(buffer);
        s_tx_history[s_tx_cursor] = {packet.seq, packet.seq + packet.data_bytes,
                                    static_cast<uint32_t>(esp_timer_get_time() / 1000), result, ref_before, ref_after};
        s_tx_cursor = (s_tx_cursor + 1) % kTxHistorySize;
        if (s_tx_count < kTxHistorySize) ++s_tx_count;
    }
    return result;
}

extern "C" esp_err_t __wrap_esp_netif_receive(esp_netif_t* netif, void* data, size_t len, void* driver_buf) {
    TcpHeader packet{};
    const uint32_t generation = s_trace_active.load(std::memory_order_acquire) && ReadTcpHeader(data, len, packet) &&
                                packet.has_ack ? MatchTrace(packet, true) : 0;
    if (!generation) return __real_esp_netif_receive(netif, data, len, driver_buf);

    // The driver can reclaim the frame inside the real call. Keep only its ACK number.
    const uint32_t ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&s_trace_lock);
    if (generation == s_generation && s_trace_active.load(std::memory_order_relaxed))
        NoteAck(s_ack.ingress, packet.ack, ms);
    portEXIT_CRITICAL(&s_trace_lock);
    const esp_err_t result = __real_esp_netif_receive(netif, data, len, driver_buf);
    portENTER_CRITICAL(&s_trace_lock);
    if (generation == s_generation && s_trace_active.load(std::memory_order_relaxed)) {
        if (result == ESP_OK) NoteAck(s_ack.queued, packet.ack, ms);
        s_ack.last_result = result;
    }
    portEXIT_CRITICAL(&s_trace_lock);
    return result;
}

TcpWifiTrace::TcpWifiTrace(esp_tls_t* tls) {
    TraceFlow flow{};
    armed_ = esp_tls_get_conn_sockfd(tls, &flow.fd) == ESP_OK && flow.fd >= 0 &&
             tcpip_api_call(&ArmTrace, &flow) == ERR_OK;
    if (!armed_) ESP_LOGW(kTag, "Voice TCP link trace unavailable");
}

TcpWifiTrace::~TcpWifiTrace() {
    if (armed_) s_trace_active.store(false, std::memory_order_release);
}

void LogTcpStall(esp_tls_t* tls, TcpStallPhase phase, TcpStallStart& start) {
    const bool at_start = phase == TcpStallPhase::kStart;
    const char* label = at_start ? "start" : phase == TcpStallPhase::kDuring ? "during" :
                        phase == TcpStallPhase::kResume ? "resume" : "timeout";
    if (at_start) start = {};

    TcpSnapshot sample{};
    sample.stall = &start;
    const int64_t requested_us = esp_timer_get_time();
    const err_t err = SampleTcp(tls, sample);
    const auto sample_ms = static_cast<unsigned>((esp_timer_get_time() - requested_us) / 1000);
    if (err != ERR_OK) {
        ESP_LOGW(kTag, "Voice TCP write stall: phase=%s sample=unavailable lwip_err=%d sample_ms=%u",
                 label, static_cast<int>(err), sample_ms);
        return;
    }

    if (at_start) {
        start.ack = sample.ack;
        start.sampled = true;
    }
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
    const char* queue = sample.watched_ref < 0 ? "absent" : sample.watched_unsent ? "unsent" : "unacked";
    ESP_LOGI(kTag, "Voice TCP retransmit: phase=%s fd=%d segment=%d seq=%lu end=%lu tcp_ticks=%lu "
                   "rtime_ms=%d rto_ms=%d queue=%s pbuf_ref=%d unacked_busy=%u",
             label, sample.fd, start.has_segment, static_cast<unsigned long>(start.sequence),
             static_cast<unsigned long>(start.end), static_cast<unsigned long>(sample.ticks),
             sample.rtime_ms, sample.rto_ms, queue, sample.watched_ref, sample.unacked_busy);
    if (!sample.link_traced || !start.has_segment) return;
    const auto& ingress = sample.rx.ingress;
    const auto& queued = sample.rx.queued;
    ESP_LOGI(kTag, "Voice Wi-Fi TCP: phase=%s fd=%d seq=%lu end=%lu tx_history=%u tx_attempts=%u tx_ok=%u "
                   "tx_last_ms=%lu tx_last=%s tx_ref=%u->%u ack_in=%u ack_in_high=%lu ack_in_ms=%lu ack_in_covers=%d "
                   "ack_queued=%u ack_queued_high=%lu ack_queued_covers=%d ack_last_result=%s",
             label, sample.fd, static_cast<unsigned long>(start.sequence), static_cast<unsigned long>(start.end),
             sample.tx_history_count, sample.tx_attempts, sample.tx_ok, static_cast<unsigned long>(sample.tx_last.ms),
             sample.tx_attempts ? esp_err_to_name(sample.tx_last.result) : "none", sample.tx_last.ref_before,
             sample.tx_last.ref_after, ingress.count, static_cast<unsigned long>(ingress.highest),
             static_cast<unsigned long>(ingress.highest_ms), ingress.count && TCP_SEQ_LEQ(start.end, ingress.highest),
             queued.count, static_cast<unsigned long>(queued.highest), queued.count && TCP_SEQ_LEQ(start.end, queued.highest),
             ingress.count ? esp_err_to_name(sample.rx.last_result) : "none");
}

void LogTcpReadGap(esp_tls_t* tls, TcpReadGapPhase phase, TcpReadGapStart& start, unsigned gap_ms) {
    const bool at_start = phase == TcpReadGapPhase::kStart;
    const char* label = at_start ? "start" : phase == TcpReadGapPhase::kResume ? "resume" : "closed";
    if (at_start) start = {};

    TcpSnapshot sample{};
    const int64_t requested_us = esp_timer_get_time();
    const err_t err = SampleTcp(tls, sample);
    const auto sample_ms = static_cast<unsigned>((esp_timer_get_time() - requested_us) / 1000);
    const auto at_ms = static_cast<long long>(requested_us / 1000);
    if (err != ERR_OK) {
        ESP_LOGI(kTag, "Voice TCP read gap: phase=%s at_ms=%lld gap_ms=%u sample=unavailable lwip_err=%d sample_ms=%u",
                 label, at_ms, gap_ms, static_cast<int>(err), sample_ms);
        return;
    }

    if (at_start) start = {sample.recv_next, true};
    const int64_t progressed = start.sampled ? static_cast<uint32_t>(sample.recv_next - start.next) : -1LL;
    ESP_LOGI(kTag, "Voice TCP read gap: phase=%s at_ms=%lld gap_ms=%u fd=%d state=%u rcv_next=%lu advanced_bytes=%lld "
                  "recv_wnd=%u announce_wnd=%u out_of_order_bytes=%u sample_ms=%u",
             label, at_ms, gap_ms, sample.fd, sample.state, static_cast<unsigned long>(sample.recv_next),
             static_cast<long long>(progressed), sample.recv_window, sample.advertised_window,
             sample.out_of_order, sample_ms);
}

}  // namespace agentlink::realtime
