#include "cloud_realtime.h"
#include "realtime_audio.h"
#include "realtime_audio_json.h"
#include "realtime_buffer.h"
#include "realtime_recovery.h"
#include "realtime_end_log.h"
#include "cloud_credential.h"
#include "realtime_ws_frame.h"
#include "realtime_ws_stream.h"
#include "realtime_ws_upgrade.h"
#include "realtime_voice_trace.h"
#include "audio_downlink.h"
#include "agent_link_platform.h"
#include "cloud_platform.h"
#include "cloud_limits.h"
#include "cloud_memory.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "realtime_tcp_diagnostics.h"
#endif
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
#include "realtime_codec_memory.h"
#endif

#include <atomic>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <memory>
#include <new>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#if CONFIG_AGENT_LINK_REALTIME_OPUS
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "freertos/ringbuf.h"
#endif
#include "freertos/task.h"
#include "mbedtls/base64.h"
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_ESP_TLS_USING_MBEDTLS
#include "mbedtls/ssl.h"
#endif
#include "mbedtls/version.h"
#if MBEDTLS_VERSION_MAJOR >= 4
#include "psa/crypto.h"
#else
#include "mbedtls/sha1.h"
#endif
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#endif

#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#define AGENT_LINK_STATIC_VOICE_PSRAM 1
#else
#define AGENT_LINK_STATIC_VOICE_PSRAM 0
#endif

namespace {
using agentlink::realtime::AudioFormat;
using agentlink::realtime::ReadAudioFormat;
using namespace agentlink::cloud_limits;
constexpr const char* TAG = "agent_link.realtime";
#if CONFIG_IDF_TARGET_ESP32S3
constexpr uint32_t kDmaCaps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

void ReportDmaHeap(const char* phase) {
    multi_heap_info_t info{};
    heap_caps_get_info(&info, kDmaCaps);
    ESP_LOGI(TAG, "Voice DMA heap: phase=%s free=%u largest=%u min=%u",
             phase, static_cast<unsigned>(info.total_free_bytes),
             static_cast<unsigned>(info.largest_free_block),
             static_cast<unsigned>(info.minimum_free_bytes));
}
#endif
constexpr int kConnectTimeoutMs = 10000;
constexpr int kSendTimeoutMs = 3000;
constexpr int64_t kWorkerYieldUs = 20000;
constexpr int64_t kReportAudioGapUs = 150000;
constexpr size_t kMicBytes = 640;
#if CONFIG_IDF_TARGET_ESP32C3
// Negotiation accepts exactly 20 ms, decoded at 16 kHz mono PCM16.
constexpr size_t kDecodePcmBytes = kMicBytes;
#else
constexpr size_t kDecodePcmBytes = 3840;
#endif
constexpr unsigned kOpusBatchFrames = kEncodedDepth < 3 ? kEncodedDepth : 3;
constexpr size_t kUplinkBatchBytes = kMicBytes * kUplinkBatchPackets;
constexpr char kAudioPrefix[] = "{\"type\":\"input_audio.append\",\"audio\":\"";
constexpr size_t kAudioJsonBytes = sizeof(kAudioPrefix) - 1 + (kUplinkBatchBytes + 2) / 3 * 4 + 3;
constexpr int64_t kReadyTimeoutUs = 10000000;
constexpr int64_t kPreparedUseMarginUs = 10000000;
constexpr int64_t kPrepareRetryUs = 15000000;
constexpr int64_t kPrepareLeaseUs = 4000000;
#if CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32S3
#define AGENT_LINK_NATIVE_WSS 1
using VoiceTls = esp_tls_t*;
#else
#define AGENT_LINK_NATIVE_WSS 0
using VoiceTls = esp_transport_handle_t;
#endif

#if !AGENT_LINK_STATIC_VOICE_PSRAM
void* ResizeMessage(void* data, size_t bytes) {
    if (!bytes) { free(data); return nullptr; }
    void* next = heap_caps_realloc(data, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return next ? next : realloc(data, bytes);
}
#endif

struct Grant {
    char url[512];
    char token[256];
    uint32_t epoch;
    int64_t expires_us;
    bool direct;
    agentlink::realtime::AudioFormat audio;
    bool platform;
    int agent_id;
    char session_id[80];
};

struct PrepareRoute {
    int agent_id;
    char session_id[sizeof Grant::session_id];
    int64_t maintain_until_us;
    int64_t next_attempt_us;
    uint32_t generation;
};

enum class TxKind { kStart, kPcm, kEnd };
struct Tx {
    TxKind kind;
    uint16_t bytes;
    uint32_t turn;
    uint8_t data[kMicBytes];
};

struct TxScratch {
    uint8_t wire[kAudioJsonBytes + 8];
    uint8_t pcm[kUplinkBatchBytes];
    char json[kAudioJsonBytes];
    uint8_t read[kReadBytes];
};

struct Rx {
    unsigned opcode;
    size_t bytes;
    uint8_t* data;
    int64_t received_at;
    uint32_t reset;
};

struct EncodedTx {
    TxKind kind;
    uint16_t bytes;
    uint32_t turn;
    uint8_t data[1275];
#if CONFIG_IDF_TARGET_ESP32S3
    int speech = -1;
#endif
};

#if AGENT_LINK_STATIC_VOICE_PSRAM
// The single realtime worker owns these buffers. It closes the previous Call
// and waits for the codec workers before reusing any of the call storage.
constexpr size_t kRxRingBytes = 2 * (kMessageBytes + sizeof(Rx) + 32);
EXT_RAM_BSS_ATTR static uint8_t s_ws_message[kMessageBytes];
EXT_RAM_BSS_ATTR static TxScratch s_ws_scratch;
EXT_RAM_BSS_ATTR static uint8_t s_grant_storage[sizeof(Grant)];
EXT_RAM_BSS_ATTR static Tx s_tx_storage[kTxDepth];
EXT_RAM_BSS_ATTR alignas(4) static uint8_t s_rx_storage[kRxRingBytes];
static StaticQueue_t s_grant_queue, s_tx_queue;
static StaticRingbuffer_t s_rx_control;
#if CONFIG_AGENT_LINK_REALTIME_OPUS
EXT_RAM_BSS_ATTR static EncodedTx s_encoded_storage[kEncodedDepth];
EXT_RAM_BSS_ATTR static int16_t s_decoded_pcm[kDecodePcmBytes / sizeof(int16_t)];
static StaticQueue_t s_encoded_control;
#endif

// These workers sleep between calls. A reconnect hands them a new Call only
// after the old one reports completion, so their static stacks stay reusable.
bool DispatchAudioWorker(TaskHandle_t task, void* call) {
    static_assert(sizeof(uintptr_t) == sizeof(uint32_t), "Task notification must hold a Call pointer");
    return task && xTaskNotify(task, reinterpret_cast<uintptr_t>(call), eSetValueWithoutOverwrite) == pdPASS;
}
#endif

struct WssAddress {
    char host[128];
    char path[384];
    int  port = 443;
};

#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
// Only the realtime worker accesses this ticket. It retains no socket and
// can only resume a previously authenticated connection to the same origin.
struct WssTicket {
    std::unique_ptr<esp_tls_client_session_t, decltype(&esp_tls_free_client_session)>
        session{nullptr, esp_tls_free_client_session};
    char host[sizeof WssAddress::host]{};
    int port = 0;
    int64_t expires_us = 0;

    esp_tls_client_session_t* For(const WssAddress& addr) {
        if (esp_timer_get_time() >= expires_us || port != addr.port || strcmp(host, addr.host) != 0) {
            session.reset();
        }
        return session.get();
    }

    void Save(const WssAddress& addr, esp_tls_t* tls) {
        session.reset(esp_tls_get_client_session(tls));
        if (!session) return;
        memcpy(host, addr.host, sizeof host);
        port = addr.port;
        expires_us = esp_timer_get_time() + 10 * 60 * 1000000LL;
        ESP_LOGI(TAG, "Voice TLS ticket stored for next call");
    }
};
WssTicket s_wss_ticket;
#endif

QueueHandle_t              s_grants = nullptr;
bool                       s_grants_caps = false;
std::atomic<TaskHandle_t>  s_task{nullptr};
std::atomic<bool>          s_run{false};
std::atomic<bool>          s_ready{false};
std::atomic<bool>          s_busy{false};
std::atomic<bool>          s_recovering{false};
std::atomic<bool>          s_mute{false};
std::atomic<bool>          s_continuous{false};
std::atomic<uint32_t>      s_epoch{0};
std::atomic<uint32_t>      s_reset{0};
portMUX_TYPE               s_audio_reset_lock = portMUX_INITIALIZER_UNLOCKED;
agent_audio_reset_reason_t s_audio_reset_reason = AGENT_AUDIO_RESET_UNKNOWN;
// Odd while S3 platform microphone input is open. The value also tags queued PCM and Opus.
std::atomic<uint32_t>      s_capture_turn{0};
std::mutex                s_mutex;
PrepareRoute              s_prepare = {};
Grant                     s_prepared = {};
QueueHandle_t             s_tx = nullptr;
bool                      s_tx_caps = false;
agent_stream_t            s_input = AGENT_STREAM_KIND_COUNT;
cloud_realtime_callbacks_t s_cb = {};
agent_link_platform_user_transcript_cb_t s_user_transcript = nullptr;
agent_link_platform_voice_timing_cb_t s_voice_timing = nullptr;
#if CONFIG_IDF_TARGET_ESP32S3
agent_link_platform_voice_detector_t s_voice_detector = nullptr;
agent_link_platform_voice_trace_cb_t s_voice_trace = nullptr;
#endif

// Diagnostics for the continuous uplink. Only counters and timings are retained.
std::atomic<uint32_t> s_tx_sent_frames{0};
std::atomic<uint32_t> s_tx_sent_messages{0};
std::atomic<uint32_t> s_tx_write_calls{0};
std::atomic<uint32_t> s_tx_bytes{0}, s_rx_bytes{0}, s_decoded_samples{0};
std::atomic<uint32_t> s_tx_send_ms{0}, s_tx_encode_max_us{0};
std::atomic<uint32_t> s_downlink_bytes{0}, s_media_stack{0}, s_encoder_stack{0};
std::atomic<uint32_t> s_encoded_pending{0};
std::atomic<uint32_t> s_tx_send_max_ms{0};
std::atomic<uint32_t> s_tx_would_block{0}, s_tx_partial{0}, s_tx_select0_accepted{0};
std::atomic<uint32_t> s_worker_phase{0};
std::atomic<uint32_t> s_worker_phase_since_ms{0};
uint32_t s_diag_last_ms = 0;
uint32_t s_diag_mic_frames = 0;
uint32_t s_diag_dropped_frames = 0;
uint32_t s_diag_prev_sent_frames = 0;
uint32_t s_diag_prev_sent_messages = 0;
uint32_t s_diag_prev_tx_bytes = 0, s_diag_prev_rx_bytes = 0, s_diag_prev_samples = 0;

#if CONFIG_IDF_TARGET_ESP32S3
int DetectVoiceFrame(const uint8_t* pcm, size_t bytes) {
    return s_voice_trace && s_voice_detector && (s_capture_turn.load() & 1)
        ? s_voice_detector(pcm, bytes) : -1;
}

void TraceCloudVoice(agent_link_platform_voice_trace_kind_t kind, int64_t at,
                     const char* event, const char* text = nullptr) {
    const uint32_t stream = s_capture_turn.load();
    if (!s_voice_trace || !s_ready || s_mute || !(stream & 1)) return;
    const agent_link_platform_voice_trace_t trace{
        .kind = kind, .at_us = at, .stream = stream,
        .last_frame = s_tx_sent_frames.load(), .event = event, .text = text,
    };
    s_voice_trace(&trace);
}

void TraceCloudControl(const char* event, const cJSON* json, int64_t at) {
    if (!s_voice_trace || !event[0] || s_mute || !(s_capture_turn.load() & 1)) return;
    const bool partial = !strcmp(event, "transcript.partial");
    if (partial || !strcmp(event, "transcript.final")) {
        const auto* role = cJSON_GetObjectItemCaseSensitive(json, "role");
        const auto* text = cJSON_GetObjectItemCaseSensitive(json, "text");
        if (cJSON_IsString(role) && !strcmp(role->valuestring, "user") && cJSON_IsString(text))
            TraceCloudVoice(partial ? AGENT_LINK_VOICE_TRACE_PARTIAL : AGENT_LINK_VOICE_TRACE_FINAL,
                            at, event, text->valuestring);
    } else if (!strcmp(event, "response.started")) {
        TraceCloudVoice(AGENT_LINK_VOICE_TRACE_RESPONSE_START, at, event);
    } else if (strncmp(event, "response.", 9) && strncmp(event, "session.", 8) &&
               strncmp(event, "task.", 5) && strcmp(event, "error")) {
        TraceCloudVoice(agentlink::realtime::CloudVoiceEvent(event, json), at, event);
    }
}
#endif

void SetWorkerPhase(uint32_t phase) {
    s_worker_phase_since_ms.store(static_cast<uint32_t>(esp_timer_get_time() / 1000));
    s_worker_phase.store(phase);
}

// Called with s_mutex held from the microphone task.
void ReportUplink() {
    if (!s_continuous || !s_diag_last_ms) return;
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (now - s_diag_last_ms < 5000) return;
    const uint32_t sent_frames = s_tx_sent_frames.load();
    const uint32_t sent_messages = s_tx_sent_messages.load();
    const uint32_t tx_bytes = s_tx_bytes.load(), rx_bytes = s_rx_bytes.load();
    const uint32_t samples = s_decoded_samples.load();
    const uint32_t interval = now - s_diag_last_ms;
    const uint32_t messages = sent_messages - s_diag_prev_sent_messages;
    const uint32_t send_ms = s_tx_send_ms.exchange(0);
    const uint32_t writes = s_tx_write_calls.exchange(0);
    const uint32_t max_send_ms = s_tx_send_max_ms.exchange(0);
    const uint32_t max_encode_us = s_tx_encode_max_us.exchange(0);
    const uint32_t phase = s_worker_phase.load();
    const uint32_t phase_age = now - s_worker_phase_since_ms.load();
    constexpr const char* phases[] = {"loop", "send", "poll", "read", "handle"};
    const char* phase_name = phase < sizeof phases / sizeof phases[0] ? phases[phase] : "unknown";
    ESP_LOGD(TAG,
             "voice %ums: mic=%u tx=%u drop=%u queue=%ums ws=%u writes=%u tx=%uB/s rx=%uB/s "
             "decoded=%ums send=%u/%ums enc=%uus pending=%uB worker=%s/%ums stack=%u/%u/%uB",
             static_cast<unsigned>(now - s_diag_last_ms), static_cast<unsigned>(s_diag_mic_frames),
             static_cast<unsigned>(sent_frames - s_diag_prev_sent_frames),
             static_cast<unsigned>(s_diag_dropped_frames),
             static_cast<unsigned>((uxQueueMessagesWaiting(s_tx) + s_encoded_pending.load()) * 20),
             static_cast<unsigned>(messages),
             static_cast<unsigned>(writes),
             static_cast<unsigned>((tx_bytes - s_diag_prev_tx_bytes) * 1000 / interval),
             static_cast<unsigned>((rx_bytes - s_diag_prev_rx_bytes) * 1000 / interval),
             static_cast<unsigned>((samples - s_diag_prev_samples) / 16),
             static_cast<unsigned>(writes ? send_ms / writes : 0),
             static_cast<unsigned>(max_send_ms),
             static_cast<unsigned>(max_encode_us),
             static_cast<unsigned>(s_downlink_bytes.load()),
             phase_name,
             static_cast<unsigned>(phase_age),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
             static_cast<unsigned>(s_media_stack.load()), static_cast<unsigned>(s_encoder_stack.load()));
#if CONFIG_IDF_TARGET_ESP32C3
    ESP_LOGD(TAG, "voice memory: internal free=%u largest=%u minimum=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
#endif
    const unsigned waiting = s_tx_would_block.exchange(0);
    const unsigned partial = s_tx_partial.exchange(0);
    const unsigned low_water = s_tx_select0_accepted.exchange(0);
    if (waiting || partial || low_water)
        ESP_LOGD(TAG, "uplink TLS progress: would_block=%u partial=%u select0_accepted=%u",
                 waiting, partial, low_water);
    s_diag_last_ms = now;
    s_diag_mic_frames = s_diag_dropped_frames = 0;
    s_diag_prev_sent_frames = sent_frames;
    s_diag_prev_sent_messages = sent_messages;
    s_diag_prev_tx_bytes = tx_bytes;
    s_diag_prev_rx_bytes = rx_bytes;
    s_diag_prev_samples = samples;
}

bool ParseWss(const char* url, WssAddress* out) {
    if (!url || strncmp(url, "wss://", 6) != 0 || strchr(url, '#')) return false;
    const char* authority = url + 6;
    const char* slash = strchr(authority, '/');
    if (!slash) return false;
    const char* colon = static_cast<const char*>(memchr(authority, ':', slash - authority));
    const char* host_end = colon ? colon : slash;
    const size_t host_len = static_cast<size_t>(host_end - authority);
    const size_t path_len = strlen(slash);
    if (!host_len || host_len >= sizeof out->host || path_len >= sizeof out->path) return false;
    for (const char* p = authority; p < host_end; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (!std::isalnum(c) && c != '-' && c != '.') return false;
    }
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(slash); *p; ++p) {
        if (*p <= 0x20 || *p == 0x7f) return false;
    }
    memcpy(out->host, authority, host_len);
    out->host[host_len] = '\0';
    memcpy(out->path, slash, path_len + 1);
    if (colon) {
        int port = 0;
        if (colon + 1 == slash) return false;
        for (const char* p = colon + 1; p < slash; ++p) {
            if (*p < '0' || *p > '9') return false;
            port = port * 10 + (*p - '0');
            if (port > 65535) return false;
        }
        if (!port) return false;
        out->port = port;
    }
    return true;
}

#if CONFIG_IDF_TARGET_ESP32S3
constexpr int64_t kVoiceDnsCacheUs = 60 * 1000000LL;
std::mutex s_voice_dns_mutex;
struct {
    char host[sizeof WssAddress::host];
    char ip[IPADDR_STRLEN_MAX];
    int64_t expires_us;
    uint32_t generation;
} s_voice_dns = {};

struct VoiceDnsQuery {
    char host[sizeof WssAddress::host];
    int64_t start_us;
    uint32_t generation;
};

bool CopyVoiceDns(const char* host, char (&ip)[IPADDR_STRLEN_MAX]) {
    std::lock_guard<std::mutex> lock(s_voice_dns_mutex);
    if (s_voice_dns.expires_us <= esp_timer_get_time() || strcmp(s_voice_dns.host, host) != 0) return false;
    memcpy(ip, s_voice_dns.ip, sizeof ip);
    return true;
}

void VoiceDnsDone(const char*, const ip_addr_t* addr, void* context) {
    std::unique_ptr<VoiceDnsQuery> query(static_cast<VoiceDnsQuery*>(context));
    char ip[IPADDR_STRLEN_MAX]{};
    if (addr && ipaddr_ntoa_r(addr, ip, sizeof ip)) {
        std::lock_guard<std::mutex> lock(s_voice_dns_mutex);
        if (query->generation == s_voice_dns.generation) {
            memcpy(s_voice_dns.host, query->host, sizeof s_voice_dns.host);
            memcpy(s_voice_dns.ip, ip, sizeof s_voice_dns.ip);
            s_voice_dns.expires_us = esp_timer_get_time() + kVoiceDnsCacheUs;
        }
    }
    ESP_LOGI(TAG, "Voice DNS prefetch: result=%s ms=%u", addr ? "resolved" : "failed",
             static_cast<unsigned>((esp_timer_get_time() - query->start_us) / 1000));
}

void ResolveVoiceDns(void* context) {
    auto* query = static_cast<VoiceDnsQuery*>(context);
    ip_addr_t addr;
    const err_t result = dns_gethostbyname(query->host, &addr, &VoiceDnsDone, query);
    if (result != ERR_INPROGRESS) VoiceDnsDone(query->host, result == ERR_OK ? &addr : nullptr, query);
}

void PrefetchVoiceDns(const char* url) {
    WssAddress addr{};
    if (!ParseWss(url, &addr)) return;
    auto* query = new (std::nothrow) VoiceDnsQuery{{}, esp_timer_get_time(), 0};
    if (!query) return;
    memcpy(query->host, addr.host, sizeof query->host);
    {
        std::lock_guard<std::mutex> lock(s_voice_dns_mutex);
        query->generation = s_voice_dns.generation;
    }
    // Enqueue a DNS query on lwIP without delaying the next call or opening a socket.
    if (tcpip_try_callback(&ResolveVoiceDns, query) != ERR_OK) delete query;
}
#endif

bool SafeToken(const char* token) {
    if (!token || !*token || strlen(token) >= sizeof Grant::token) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(token); *p; ++p) {
        if (*p < 0x20 || *p == 0x7f) return false;
    }
    return true;
}

bool Current(uint32_t epoch) { return s_run && s_epoch == epoch; }
void SetRecovering(uint32_t epoch, bool recovering) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (Current(epoch)) s_recovering.store(recovering);
}

class VoiceWifiPower {
public:
    VoiceWifiPower() {
        changed_ = esp_wifi_get_ps(&previous_) == ESP_OK && previous_ != WIFI_PS_NONE &&
                   esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
        if (changed_) ESP_LOGD(TAG, "WiFi power saving suspended for voice");
    }
    ~VoiceWifiPower() { if (changed_) (void)esp_wifi_set_ps(previous_); }
private:
    wifi_ps_type_t previous_ = WIFI_PS_MIN_MODEM;
    bool changed_ = false;
};

#if CONFIG_AGENT_LINK_REALTIME_DIRECT
bool DirectConfigValid() {
    const char* base = CONFIG_AGENT_LINK_REALTIME_BASE_URL;
    const char* session = CONFIG_AGENT_LINK_REALTIME_SESSION_ID;
    if (strncmp(base, "http://", 7) != 0 && strncmp(base, "https://", 8) != 0) return false;
    if (!*session || !SafeToken(CONFIG_AGENT_LINK_REALTIME_USER_API_KEY)) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(session); *p; ++p) {
        if (!std::isalnum(*p) && *p != '_' && *p != '-') return false;
    }
    return true;
}

// Separate from the device gateway client; its credential is not a User API Key.
esp_err_t DirectHttp(esp_http_client_method_t method, const char* suffix,
                     char* response, size_t capacity, int* status) {
    agentlink::cloud::NetworkMemoryLock memory;
    char url[512];
    const char* base = CONFIG_AGENT_LINK_REALTIME_BASE_URL;
    size_t base_len = strlen(base);
    while (base_len && base[base_len - 1] == '/') --base_len;
    const int length = snprintf(url, sizeof url, "%.*s/api/v1/user/sessions/%s/realtime%s",
            static_cast<int>(base_len), base, CONFIG_AGENT_LINK_REALTIME_SESSION_ID, suffix);
    if (length <= 0 || length >= static_cast<int>(sizeof url)) return ESP_ERR_INVALID_SIZE;

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = method;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    char authorization[sizeof Grant::token + 16];
    snprintf(authorization, sizeof authorization, "Bearer %s", CONFIG_AGENT_LINK_REALTIME_USER_API_KEY);
    esp_err_t result = esp_http_client_set_header(client, "Authorization", authorization);
    memset(authorization, 0, sizeof authorization);
    if (result == ESP_OK) result = esp_http_client_set_header(client, "Content-Type", "application/json");
    const bool post = method == HTTP_METHOD_POST;
    const char* body = "{}";
#if CONFIG_AGENT_LINK_REALTIME_OPUS
    if (!*suffix) body = "{\"audio\":{\"input\":{\"encoding\":\"pcm16\",\"sampleRate\":16000,\"channels\":1},"
                         "\"output\":{\"encoding\":\"opus\",\"sampleRate\":24000,\"channels\":1,\"packetDurationMs\":20}}}";
#endif
    const int body_bytes = static_cast<int>(strlen(body));
    if (result == ESP_OK) result = esp_http_client_open(client, post ? body_bytes : 0);
    if (result == ESP_OK && post && esp_http_client_write(client, body, body_bytes) != body_bytes) result = ESP_FAIL;
    if (result == ESP_OK && esp_http_client_fetch_headers(client) < 0) result = ESP_FAIL;
    if (result == ESP_OK) {
        *status = esp_http_client_get_status_code(client);
        const int bytes = esp_http_client_read_response(client, response, static_cast<int>(capacity - 1));
        if (bytes < 0 || !esp_http_client_is_complete_data_received(client)) result = ESP_FAIL;
        else response[bytes] = '\0';
    }
    esp_http_client_cleanup(client);
    return result;
}

bool FetchDirectGrant(Grant* grant, const char** hint) {
    if (s_cb.state) s_cb.state(false, "Requesting voice access");
    auto* response = static_cast<char*>(heap_caps_calloc(1, 8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!response) response = static_cast<char*>(calloc(1, 8192));
    if (!response) { *hint = "No memory for voice access"; return false; }
    int status = 0;
    AudioFormat audio;
#if CONFIG_AGENT_LINK_REALTIME_OPUS
    audio.output_opus = true;
#endif
    esp_err_t result = DirectHttp(HTTP_METHOD_POST, "", response, 8192, &status);
    ESP_LOGI(TAG, "voice grant request: HTTP %d (%s)", status, esp_err_to_name(result));
    if (result == ESP_OK && status == 409 && Current(grant->epoch)) {
        result = DirectHttp(HTTP_METHOD_GET, "", response, 8192, &status);
        cJSON* existing = result == ESP_OK && status == 200 ? cJSON_Parse(response) : nullptr;
        const cJSON* data = cJSON_GetObjectItemCaseSensitive(existing, "data");
        const bool known = ReadAudioFormat(cJSON_IsObject(data) ? data : existing, &audio, false);
        cJSON_Delete(existing);
        if (!known) {
            memset(response, 0, 8192);
            free(response);
            *hint = "Cannot resume this audio format. End the previous call";
            return false;
        }
        result = DirectHttp(HTTP_METHOD_POST, "/connect-tokens", response, 8192, &status);
        ESP_LOGI(TAG, "voice token request: HTTP %d (%s)", status, esp_err_to_name(result));
    }
    bool valid = false;
    if (result == ESP_OK && status == 201) {
        cJSON* json = cJSON_Parse(response);
        const cJSON* body = json;
        const cJSON* data = cJSON_GetObjectItemCaseSensitive(json, "data");
        if (cJSON_IsObject(data)) body = data;
        const cJSON* url = cJSON_GetObjectItemCaseSensitive(body, "websocketUrl");
        const cJSON* token = cJSON_GetObjectItemCaseSensitive(body, "connectToken");
        const cJSON* connection = cJSON_GetObjectItemCaseSensitive(body, "realtimeConnection");
        const bool has_format = cJSON_HasObjectItem(connection, "audio");
        WssAddress parsed = {};
        valid = cJSON_IsString(url) && cJSON_IsString(token) &&
                strlen(url->valuestring) < sizeof grant->url && ParseWss(url->valuestring, &parsed) &&
                SafeToken(token->valuestring) && (!has_format || ReadAudioFormat(body, &audio, false));
        if (valid) {
            snprintf(grant->url, sizeof grant->url, "%s", url->valuestring);
            snprintf(grant->token, sizeof grant->token, "%s", token->valuestring);
            grant->audio = audio;
            ESP_LOGI(TAG, "voice grant accepted; connecting to WSS");
        }
        cJSON_Delete(json);
    }
    if (!valid) {
        *hint = result != ESP_OK ? "Voice API network error. Tap Connect to retry"
              : status == 401 ? "Voice API rejected the User API Key (401)"
              : status == 403 ? "Voice API access denied (403)"
              : status == 404 ? "Voice API session not found (404)"
              : status == 409 ? "Voice session is busy (409)"
              : status == 201 ? "Voice API returned an invalid WSS grant"
                              : "Voice API rejected the request. Check the log";
    }
    memset(response, 0, 8192);
    free(response);
    return valid;
}

void EndDirectSession() {
    char response[512] = {};
    int status = 0;
    const esp_err_t result = DirectHttp(HTTP_METHOD_DELETE, "", response, sizeof response, &status);
    ESP_LOGI(TAG, "voice session cleanup: HTTP %d (%s)", status, esp_err_to_name(result));
    memset(response, 0, sizeof response);
}
#endif

void LogTransportFailure(VoiceTls transport, const char* operation,
                         int result, int saved_errno) {
    int tls_code = 0, tls_flags = 0;
#if AGENT_LINK_NATIVE_WSS
    esp_tls_error_handle_t error = nullptr;
    if (!transport) return;
    (void)esp_tls_get_error_handle(transport, &error);
    int socket_error = 0, socket = -1;
    socklen_t size = sizeof socket_error;
    if (esp_tls_get_conn_sockfd(transport, &socket) == ESP_OK && socket >= 0)
        (void)getsockopt(socket, SOL_SOCKET, SO_ERROR, &socket_error, &size);
#else
    const auto error = esp_transport_get_error_handle(transport);
    const int socket_error = esp_transport_get_errno(transport);
#endif
    const esp_err_t tls_error = error
        ? esp_tls_get_and_clear_last_error(error, &tls_code, &tls_flags) : ESP_OK;
    wifi_ap_record_t ap = {};
    const bool associated = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    ESP_LOGW(TAG, "%s: result=%d errno=%d socket_error=%d tls=%s code=%d flags=0x%x "
                  "wifi=%s rssi=%d internal=%u largest=%u",
             operation, result, saved_errno, socket_error, esp_err_to_name(tls_error),
             tls_code, tls_flags, associated ? "associated" : "disconnected", associated ? ap.rssi : 0,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

struct WsSender {
    VoiceTls tls;
    uint8_t* wire;
    size_t capacity;
    int64_t last_slow_log = 0;
#if AGENT_LINK_NATIVE_WSS
    agentlink::realtime::PendingWrite pending;
    unsigned audio_frames = 0, audio_messages = 0;
    uint32_t audio_turn = 0;
    size_t audio_bytes = 0;
    int64_t queued_at = 0;
    uint32_t receive_at_first_wait = 0;
    int64_t first_wait_at = 0;
#if CONFIG_IDF_TARGET_ESP32S3
    agentlink::realtime::VoiceFrameCounts voice_counts;
    int64_t last_data_received_ms = 0;
    int64_t last_pump_at = 0;
    bool wait_extension_logged = false;
    unsigned peak_uplink_queue_ms = 0;
    agentlink::realtime::TcpStallStart tcp_stall_start;
    struct {
        int first_tls_wait = 0;
        int last_tls_wait = 0;
        int first_writable = -1;
        int64_t first_loop_gap_us = 0;
        int64_t max_loop_gap_us = 0;
        unsigned want_read = 0;
        unsigned want_write = 0;
        bool start_logged = false;
    } write_wait;
#endif
#endif
};

int PollWritable(VoiceTls tls) {
#if AGENT_LINK_NATIVE_WSS
    int socket = -1;
    if (esp_tls_get_conn_sockfd(tls, &socket) != ESP_OK || socket < 0) return -1;
    fd_set writes; FD_ZERO(&writes); FD_SET(socket, &writes);
    timeval timeout{};
    return select(socket + 1, nullptr, &writes, nullptr, &timeout);
#else
    return esp_transport_poll_write(tls, 0);
#endif
}

#if AGENT_LINK_NATIVE_WSS
bool TlsWouldBlock(int result) {
    return result == ESP_TLS_ERR_SSL_WANT_READ || result == ESP_TLS_ERR_SSL_WANT_WRITE;
}

#if CONFIG_IDF_TARGET_ESP32S3
const char* TlsWaitName(int result) {
    if (result == ESP_TLS_ERR_SSL_WANT_READ) return "WANT_READ";
    if (result == ESP_TLS_ERR_SSL_WANT_WRITE) return "WANT_WRITE";
    return "none";
}

bool LogWriteWaitStart(WsSender& sender, int64_t now_us) {
    auto& wait = sender.write_wait;
    // Remember the first sample, but avoid logging routine brief backpressure.
    if (!sender.first_wait_at || wait.start_logged || now_us - sender.first_wait_at < kReportAudioGapUs) return false;
    wait.start_logged = true;
    ESP_LOGI(TAG, "Voice TLS write stalled: tls_wait=%s socket_writable=%d loop_gap_us=%lld",
             TlsWaitName(wait.first_tls_wait), wait.first_writable, static_cast<long long>(wait.first_loop_gap_us));
    return true;
}
#endif

bool PumpWire(WsSender& sender) {
#if CONFIG_IDF_TARGET_ESP32S3
    const int64_t pump_at = esp_timer_get_time();
    const int64_t loop_gap_us = sender.last_pump_at ? pump_at - sender.last_pump_at : 0;
    sender.last_pump_at = pump_at;
    if (sender.first_wait_at)
        sender.write_wait.max_loop_gap_us = std::max(sender.write_wait.max_loop_gap_us, loop_gap_us);
#endif
    if (!sender.pending.Busy()) return true;
    std::unique_lock<std::mutex> capture_lock(s_mutex, std::defer_lock);
    if (sender.audio_turn) {
        capture_lock.lock();
        // An untouched frame can be discarded. Once TLS has prepared a record
        // or accepted part of a frame, it must finish with the original bytes.
        if (sender.audio_turn != s_capture_turn.load() &&
            !sender.first_wait_at && !sender.pending.Offset()) {
            memset(sender.wire, 0, sender.pending.Size());
            sender.pending = {};
            sender.audio_frames = sender.audio_messages = 0;
            sender.audio_bytes = 0;
            sender.audio_turn = 0;
            return true;
        }
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
#if CONFIG_IDF_TARGET_ESP32S3
    constexpr int64_t kNoDownlinkStallMs = 5000;
    constexpr int64_t kActiveDownlinkStallMs = 6000;
    int64_t stall_limit_ms = kNoDownlinkStallMs;
    if (sender.first_wait_at) {
        const unsigned queued_ms = (uxQueueMessagesWaiting(s_tx) + s_encoded_pending.load()) * 20;
        sender.peak_uplink_queue_ms = std::max(sender.peak_uplink_queue_ms, queued_ms);
    }
    const bool receiving = sender.audio_messages && sender.first_wait_at &&
                           sender.last_data_received_ms >= sender.first_wait_at / 1000 &&
                           now_ms - sender.last_data_received_ms <= 1000;
    if (receiving) {
        stall_limit_ms = kActiveDownlinkStallMs;
        if (!sender.wait_extension_logged && sender.pending.StalledMs(now_ms) >= kNoDownlinkStallMs) {
            sender.wait_extension_logged = true;
            ESP_LOGI(TAG, "Voice TLS write waiting: downlink active, limit_ms=%lld",
                     static_cast<long long>(stall_limit_ms));
        }
    }
#else
    constexpr int64_t stall_limit_ms = agentlink::realtime::PendingWrite::kStallMs;
#endif
    const auto timed_out = [&](int64_t at_ms) {
#if CONFIG_IDF_TARGET_ESP32S3
        if (capture_lock.owns_lock()) capture_lock.unlock();
        if (LogWriteWaitStart(sender, esp_timer_get_time())) {
            agentlink::realtime::LogTcpStall(sender.tls, agentlink::realtime::TcpStallPhase::kStart,
                                            sender.tcp_stall_start);
        }
#endif
        ESP_LOGW(TAG, "voice TLS write expired: no_progress_ms=%lld age_ms=%lld limit_ms=%lld pending=%u offset=%u rx_while_waiting=%u",
                 static_cast<long long>(sender.pending.StalledMs(at_ms)),
                 static_cast<long long>(sender.pending.AgeMs(at_ms)), static_cast<long long>(stall_limit_ms),
                 static_cast<unsigned>(sender.pending.Remaining()),
                 static_cast<unsigned>(sender.pending.Offset()),
                 sender.first_wait_at ? static_cast<unsigned>(s_rx_bytes.load() - sender.receive_at_first_wait) : 0);
#if CONFIG_IDF_TARGET_ESP32S3
        const auto& wait = sender.write_wait;
        const int writable = PollWritable(sender.tls);
        ESP_LOGW(TAG, "Voice TLS write backlog: peak_uplink_ms=%u downlink_pending_bytes=%u microphone=%s "
                      "tls_wait=%s want_read=%u want_write=%u socket_writable=%d loop_gap_us=%lld max_loop_gap_us=%lld",
                 sender.peak_uplink_queue_ms, static_cast<unsigned>(s_downlink_bytes.load()),
                 (s_capture_turn.load() & 1) ? "on" : "off", TlsWaitName(wait.last_tls_wait),
                 wait.want_read, wait.want_write, writable, static_cast<long long>(loop_gap_us),
                 static_cast<long long>(std::max(wait.max_loop_gap_us, loop_gap_us)));
        agentlink::realtime::LogTcpStall(sender.tls, agentlink::realtime::TcpStallPhase::kTimeout,
                                        sender.tcp_stall_start);
#endif
        LogTransportFailure(sender.tls, "write progress timeout", 0, 0);
        return false;
    };
    if (sender.pending.Expired(now_ms, stall_limit_ms)) return timed_out(now_ms);
    // Retry exactly the retained suffix, including after WANT_READ/WANT_WRITE.
    // This socket stays nonblocking after the verified async TLS handshake.
    // select() has a TCP low-water threshold, not a promise that this small
    // TLS record cannot fit. Use it only to observe; the nonblocking write is
    // authoritative and retains the same bytes across WANT_READ/WANT_WRITE.
    const int writable = PollWritable(sender.tls);
    const size_t remaining = sender.pending.Remaining();
    errno = 0;
    const int sent = esp_tls_conn_write(sender.tls, sender.wire + sender.pending.Offset(), remaining);
#if CONFIG_IDF_TARGET_ESP32S3 && MBEDTLS_VERSION_MAJOR >= 4
    if (sent == PSA_ERROR_GENERIC_ERROR) ReportDmaHeap("tls_write_failed");
#endif
    const bool waiting = TlsWouldBlock(sent);
    if (waiting) {
        s_tx_would_block.fetch_add(1);
        if (!sender.first_wait_at) {
            sender.first_wait_at = esp_timer_get_time();
            sender.receive_at_first_wait = s_rx_bytes.load();
#if CONFIG_IDF_TARGET_ESP32S3
            sender.write_wait.first_tls_wait = sent;
            sender.write_wait.first_writable = writable;
            sender.write_wait.first_loop_gap_us = loop_gap_us;
            sender.write_wait.max_loop_gap_us = loop_gap_us;
            sender.tcp_stall_start = {};
#endif
        }
#if CONFIG_IDF_TARGET_ESP32S3
        sender.write_wait.last_tls_wait = sent;
        if (sent == ESP_TLS_ERR_SSL_WANT_READ) ++sender.write_wait.want_read;
        else ++sender.write_wait.want_write;
#endif
    }
    if (sent > 0 && static_cast<size_t>(sent) < remaining) s_tx_partial.fetch_add(1);
    if (sent > 0 && writable == 0) s_tx_select0_accepted.fetch_add(1);
    const int64_t finished_us = esp_timer_get_time();
    const int64_t finished_ms = finished_us / 1000;
    if (!sender.pending.Advance(sent, waiting, finished_ms, stall_limit_ms)) {
        if (waiting && sender.pending.Expired(finished_ms, stall_limit_ms)) return timed_out(finished_ms);
        LogTransportFailure(sender.tls, "write", sent, errno);
        return false;
    }
#if CONFIG_IDF_TARGET_ESP32S3
    const bool report_tcp_start = LogWriteWaitStart(sender, finished_us);
#endif
    if (!sender.pending.Busy()) {
        const uint32_t waited_rx = s_rx_bytes.load() - sender.receive_at_first_wait;
#if CONFIG_IDF_TARGET_ESP32S3
        const auto& wait = sender.write_wait;
        if (wait.start_logged) {
            ESP_LOGI(TAG, "Voice TLS write resumed: wait_ms=%u rx_while_waiting=%u peak_uplink_ms=%u "
                          "tls_wait=%s want_read=%u want_write=%u socket_writable=%d loop_gap_us=%lld max_loop_gap_us=%lld",
                     static_cast<unsigned>((finished_us - sender.first_wait_at) / 1000),
                     static_cast<unsigned>(waited_rx), sender.peak_uplink_queue_ms, TlsWaitName(wait.last_tls_wait),
                     wait.want_read, wait.want_write, writable, static_cast<long long>(loop_gap_us),
                     static_cast<long long>(wait.max_loop_gap_us));
        }
        sender.write_wait = {};
        sender.peak_uplink_queue_ms = 0;
#else
        if (sender.first_wait_at && waited_rx && esp_timer_get_time() - sender.first_wait_at >= kReportAudioGapUs) {
            ESP_LOGI(TAG, "Voice TLS write resumed: wait_ms=%u rx_while_waiting=%u",
                     static_cast<unsigned>((esp_timer_get_time() - sender.first_wait_at) / 1000),
                     static_cast<unsigned>(waited_rx));
        }
#endif
        sender.first_wait_at = 0;
#if CONFIG_IDF_TARGET_ESP32S3
        sender.wait_extension_logged = false;
#endif
        memset(sender.wire, 0, sender.pending.Size());
        if (sender.audio_messages && (!sender.audio_turn || sender.audio_turn == s_capture_turn.load())) {
            const uint32_t elapsed = static_cast<uint32_t>((esp_timer_get_time() - sender.queued_at) / 1000);
            const uint32_t previously_sent = s_tx_sent_frames.fetch_add(sender.audio_frames);
            if (s_continuous.load() && previously_sent == 0)
                ESP_LOGI(TAG, "Voice first uplink: sent_ms=%u frames=%u",
                         static_cast<unsigned>(esp_timer_get_time() / 1000), sender.audio_frames);
            s_tx_sent_messages.fetch_add(sender.audio_messages);
            s_tx_bytes.fetch_add(static_cast<uint32_t>(sender.audio_bytes));
            s_tx_write_calls.fetch_add(1);
            s_tx_send_ms.fetch_add(elapsed);
            if (elapsed > s_tx_send_max_ms.load()) s_tx_send_max_ms.store(elapsed);
#if CONFIG_IDF_TARGET_ESP32S3
            if (s_voice_trace && (sender.audio_turn & 1)) {
                const agent_link_platform_voice_trace_t trace{
                    .kind = AGENT_LINK_VOICE_TRACE_UPLINK, .at_us = finished_us, .stream = sender.audio_turn,
                    .last_frame = previously_sent + sender.audio_frames, .frames = sender.audio_frames,
                    .classified_frames = sender.voice_counts.classified, .speech_frames = sender.voice_counts.speech,
                };
                // Serial logging must not hold up microphone queue writes.
                if (capture_lock.owns_lock()) capture_lock.unlock();
                s_voice_trace(&trace);
            }
#endif
        }
        sender.audio_frames = sender.audio_messages = 0;
        sender.audio_bytes = 0;
        sender.audio_turn = 0;
    }
#if CONFIG_IDF_TARGET_ESP32S3
    if (report_tcp_start) {
        if (capture_lock.owns_lock()) capture_lock.unlock();
        agentlink::realtime::LogTcpStall(sender.tls, agentlink::realtime::TcpStallPhase::kStart,
                                        sender.tcp_stall_start);
    }
#endif
    return true;
}
#endif

bool WriteWire(WsSender& sender, size_t expected) {
    if (!expected || expected > sender.capacity) return false;
#if AGENT_LINK_NATIVE_WSS
    if (sender.pending.Busy()) return false;
    sender.queued_at = esp_timer_get_time();
    return sender.pending.Begin(expected, sender.queued_at / 1000);
#else
    errno = 0;
    const int64_t started = esp_timer_get_time();
    const int sent = esp_transport_write(sender.tls, reinterpret_cast<const char*>(sender.wire),
                                         static_cast<int>(expected), kSendTimeoutMs);
    const int saved_errno = errno;
    const uint32_t write_ms = static_cast<uint32_t>((esp_timer_get_time() - started) / 1000);
    const bool ok = sent == static_cast<int>(expected);
    // Clear the reusable masked buffer, including session.connect credentials.
    if (expected) memset(sender.wire, 0, expected);
    if (ok) {
        const int64_t now = esp_timer_get_time();
        if (write_ms >= 200 && now - sender.last_slow_log >= 1000000) {
            sender.last_slow_log = now;
            ESP_LOGW(TAG, "slow WSS write: wire=%u write=%ums (header+payload, one TLS write)",
                     static_cast<unsigned>(expected), static_cast<unsigned>(write_ms));
        }
        return true;
    }
    // Even a negative result can follow a partial frame header/payload write.
    // Close this connection; never replay the frame on the same stream.
    ESP_LOGW(TAG, "WSS frame send failed: wire=%u returned=%d write=%ums timeout=%dms",
             static_cast<unsigned>(expected), sent, static_cast<unsigned>(write_ms), kSendTimeoutMs);
    LogTransportFailure(sender.tls, "send", sent, saved_errno);
    return false;
#endif
}

#if AGENT_LINK_NATIVE_WSS
bool PumpControl(WsSender& sender, const uint8_t* pong, size_t pong_size,
                 bool* pong_pending, int64_t* last_ping, int64_t now) {
    if (sender.pending.Busy()) return true;
    const bool reply = *pong_pending;
    if (!reply && now - *last_ping <= 15000000) return true;
    uint8_t mask[4]; esp_fill_random(mask, sizeof mask);
    const size_t size = agentlink::realtime::EncodeFrame(reply ? 10 : 9,
        reply ? reinterpret_cast<const char*>(pong) : nullptr, reply ? pong_size : 0,
        mask, sender.wire, sender.capacity);
    if (!size || !WriteWire(sender, size)) return false;
    if (reply) *pong_pending = false;
    else *last_ping = now;
    return true;
}
#endif

size_t AppendFrame(WsSender& sender, size_t offset, const char* json, size_t bytes) {
#if AGENT_LINK_NATIVE_WSS
    if (sender.pending.Busy()) return 0;
#endif
    if (offset > sender.capacity) return 0;
    uint8_t mask[4];
    esp_fill_random(mask, sizeof mask);
    return agentlink::realtime::EncodeTextFrame(json, bytes, mask,
                                               sender.wire + offset, sender.capacity - offset);
}

bool Send(WsSender& sender, const char* json, size_t bytes) {
    const size_t size = AppendFrame(sender, 0, json, bytes);
    return size && WriteWire(sender, size);
}

bool Send(WsSender& ws, const char* json) {
    return agentlink::realtime::SendControl(json, [&ws](char* frame, size_t bytes) {
        return Send(ws, frame, bytes);
    });
}

uint32_t AdvanceAudioReset(agent_audio_reset_reason_t reason) {
    portENTER_CRITICAL(&s_audio_reset_lock);
    s_audio_reset_reason = reason;
    const uint32_t seq = s_reset.fetch_add(1) + 1;
    portEXIT_CRITICAL(&s_audio_reset_lock);
    return seq;
}

void CheckResponseInterruption(const uint8_t* data, size_t bytes, [[maybe_unused]] int64_t received_at,
                               [[maybe_unused]] const char* connect_token) {
    constexpr char event_name[] = "response.interrupted";
    if (std::search(data, data + bytes, event_name, event_name + sizeof event_name - 1) == data + bytes) return;
    cJSON* json = cJSON_ParseWithLength(reinterpret_cast<const char*>(data), bytes);
    const auto* type = cJSON_GetObjectItemCaseSensitive(json, "type");
    const bool interrupted = cJSON_IsString(type) && strcmp(type->valuestring, event_name) == 0;
#if CONFIG_IDF_TARGET_ESP32S3
    char reason[96]{};
    if (interrupted && s_voice_trace) {
        const auto* field = agentlink::realtime::EndField(json, "reason", "cause");
        agentlink::realtime::SafeEndField(reason, sizeof reason, cJSON_IsString(field) ? field->valuestring : nullptr,
                                         cloud_cred_auth_key(), connect_token);
    }
#endif
    cJSON_Delete(json);
    if (!interrupted) return;
    [[maybe_unused]] const uint32_t seq = AdvanceAudioReset(AGENT_AUDIO_RESET_REMOTE_INTERRUPT);
#if CONFIG_IDF_TARGET_ESP32S3
    if (s_voice_trace) {
        const agent_link_platform_voice_trace_t trace{
            .kind = AGENT_LINK_VOICE_TRACE_RESPONSE_INTERRUPTED, .at_us = received_at,
            .stream = s_capture_turn.load(), .last_frame = s_tx_sent_frames.load(),
            .event = event_name, .text = reason, .reset_seq = seq,
        };
        s_voice_trace(&trace);
    }
#endif
}

void ResetInputLocked() {
    if (s_capture_turn.load() & 1) s_capture_turn.fetch_add(1);
    s_input = AGENT_STREAM_KIND_COUNT;
    if (s_tx) xQueueReset(s_tx);
}

void ResetInput() {
    std::lock_guard<std::mutex> lock(s_mutex);
    ResetInputLocked();
}

class Call {
public:
    Call(uint32_t epoch, AudioFormat audio, bool continuous, const char* connect_token = nullptr,
         const char* conversation_id = nullptr)
        : epoch_(epoch), continuous_(continuous), audio_(audio), input_opus_(audio.input_opus),
          connect_token_(connect_token), conversation_id_(conversation_id), created_at_(esp_timer_get_time()) {}
    ~Call() {
        Stop();
        if (encoded_queue_) {
            if (encoded_queue_caps_) vQueueDeleteWithCaps(encoded_queue_);
            else vQueueDelete(encoded_queue_);
        }
        s_encoded_pending.store(0);
#if CONFIG_IDF_TARGET_ESP32S3
        if (rx_ring_) {
#if AGENT_LINK_STATIC_VOICE_PSRAM
            vRingbufferDelete(rx_ring_);
#else
            vRingbufferDeleteWithCaps(rx_ring_);
#endif
        }
#else
        if (rx_queue_) {
            Rx packet;
            while (xQueueReceive(rx_queue_, &packet, 0) == pdTRUE) free(packet.data);
            if (rx_caps_) vQueueDeleteWithCaps(rx_queue_);
            else vQueueDelete(rx_queue_);
        }
#endif
        s_downlink_bytes.store(0);
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        if (decoder_) esp_opus_dec_close(decoder_);
        if (encoder_) esp_opus_enc_close(encoder_);
#endif
#if !AGENT_LINK_STATIC_VOICE_PSRAM
        free(decode_pcm_);
#endif
        free(silence_);
    }

    bool Prepare() {
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
        // The shared worker stack is already resident. TLS handshake scratch
        // has been released before these per-call codec states are allocated.
        ESP_LOGD(TAG, "before Opus allocation: internal free=%u largest=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        if (SharedCodec() && !codec_memory_.Reserve()) {
            ESP_LOGE(TAG, "could not reserve the %u-byte Opus workspace",
                     static_cast<unsigned>(agentlink::realtime::CodecWorkspace::kBytes));
            return false;
        }
        if ((!SharedCodec() && !OpenDecoder()) || (audio_.input_opus && !StartEncoder())) return false;
        ReportMemory("codec prepared");
#endif
        return true;
    }

    bool Start() {
#if CONFIG_IDF_TARGET_ESP32C3
        ESP_LOGD(TAG, "resident voice worker: network + codec, internal free=%u largest=%u stack_free=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        return true;
#else
#if CONFIG_IDF_TARGET_ESP32S3
        // A no-split ring needs just over twice the largest item to guarantee
        // it can accept that item at any position. Reserve it once in PSRAM.
        constexpr size_t rx_ring_bytes = 2 * (kMessageBytes + sizeof(Rx) + 32);
#if AGENT_LINK_STATIC_VOICE_PSRAM
        rx_ring_ = xRingbufferCreateStatic(kRxRingBytes, RINGBUF_TYPE_NOSPLIT, s_rx_storage, &s_rx_control);
#else
        rx_ring_ = xRingbufferCreateWithCaps(rx_ring_bytes, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if (!rx_ring_) return false;
        ESP_LOGI(TAG, "Resident WSS downlink buffer: %uB PSRAM", static_cast<unsigned>(rx_ring_bytes));
#else
        rx_queue_ = xQueueCreateWithCaps(kDownlinkDepth, sizeof(Rx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        rx_caps_ = rx_queue_ != nullptr;
        if (!rx_queue_) rx_queue_ = xQueueCreate(kDownlinkDepth, sizeof(Rx));
        if (!rx_queue_) return false;
#endif
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        constexpr unsigned stack_bytes = 24576;
#else
        constexpr unsigned stack_bytes = 8192;
#endif
#if AGENT_LINK_STATIC_VOICE_PSRAM
        EXT_RAM_BSS_ATTR alignas(16) static StackType_t media_stack[stack_bytes];
        static StaticTask_t media_tcb;
        static const TaskHandle_t media = xTaskCreateStatic(&MediaEntry, "agentlink_audio", sizeof(media_stack),
                                                             nullptr, 5, media_stack, &media_tcb);
        media_started_ = DispatchAudioWorker(media, this);
        media_caps_ = true;
#else
        media_caps_ = true;
        media_started_ = xTaskCreateWithCaps(&MediaEntry, "agentlink_audio", stack_bytes, this, 5,
                                           nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
        if (!media_started_) {
            media_caps_ = false;
            media_started_ = xTaskCreate(&MediaEntry, "agentlink_audio", stack_bytes, this, 5, nullptr) == pdPASS;
        }
#endif
        if (!media_started_)
            ESP_LOGE(TAG, "could not start downlink audio worker (%uB stack, internal free=%u largest=%u)",
                     stack_bytes,
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        else
            ESP_LOGI(TAG, "Downlink audio stack=%s; DMA heap free=%uB", media_caps_ ? "PSRAM" : "internal",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)));
        return media_started_;
#endif
    }

#if CONFIG_IDF_TARGET_ESP32C3
    void BindNetwork(WsSender* sender, TxScratch* scratch) { sender_ = sender; scratch_ = scratch; }
    bool Pump() {
        if (reset_ != s_reset.load()) ResetAudio();
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        if (!PumpEncoder()) running_.store(false);
#endif
        s_media_stack.store(uxTaskGetStackHighWaterMark(nullptr));
        return Healthy();
    }
#endif

    bool Healthy() const { return running_.load(); }
    bool TransportFailed() const { return transport_failed_; }
    bool CanPauseInput() const {
#if CONFIG_IDF_TARGET_ESP32S3
        return continuous_;
#else
        return false;
#endif
    }
    void Stop() {
        running_.store(false);
        while (media_started_ && !media_done_.load()) vTaskDelay(1);
        while (encoder_started_ && !encoder_done_.load()) vTaskDelay(1);
    }

    bool SharedCodec() const {
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
        // The Passport application already suppresses mic audio during speaker
        // playback. Keep its workers resident, but never keep both codec states
        // resident for a continuous half-duplex call.
        return continuous_ && audio_.input_opus && audio_.output_opus;
#else
        return false;
#endif
    }

    void ReportMemory(const char* phase) {
        (void)phase;
#if CONFIG_IDF_TARGET_ESP32C3
        ESP_LOGD(TAG, "voice memory %s: free=%u largest=%u minimum=%u stack_free=%u",
                 phase,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        if (SharedCodec())
            ESP_LOGD(TAG, "Opus workspace: reserved=%u peak=%u allocations=%u",
                     static_cast<unsigned>(agentlink::realtime::CodecWorkspace::kBytes),
                     static_cast<unsigned>(codec_memory_.Peak()), static_cast<unsigned>(codec_memory_.Live()));
#endif
#endif
    }

    void ClearCodecUplink() {
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
        // Only queued packets are discarded at a reply boundary. A partially
        // written TLS/WebSocket frame must finish with its original bytes.
        std::lock_guard<std::mutex> lock(s_mutex);
        xQueueReset(s_tx);
        if (encoded_queue_) xQueueReset(encoded_queue_);
        s_encoded_pending.store(0);
        shared_frames_.Reset();
        batch_gate_.Reset();
#endif
    }

    bool BeginReply() {
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
        if (!SharedCodec()) return true;
        replying_ = true;
        ClearCodecUplink();
        if (encoder_) { esp_opus_enc_close(encoder_); encoder_ = nullptr; }
        ResetDecoder();
        const bool opened = OpenDecoder();
        ReportMemory("reply decoder only");
        return opened;
#else
        return true;
#endif
    }

    bool EndReply() {
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
        if (!SharedCodec()) return true;
        replying_ = false;
        ResetDecoder();
        ClearCodecUplink();
        const bool opened = StartEncoder();
        ReportMemory("listening encoder only");
        return opened;
#else
        return true;
#endif
    }

    bool TxPending() {
        if (input_opus_.load()) {
            const unsigned queued = encoded_queue_ ? uxQueueMessagesWaiting(encoded_queue_) : 0;
            const bool due = batch_gate_.Ready(queued, esp_timer_get_time() / 1000);
            if (!queued) return false;
            if (!continuous_) return true;
            if (xQueuePeek(encoded_queue_, &encoded_packet_, 0) != pdTRUE) return false;
            return encoded_packet_.kind != TxKind::kPcm || due;
        }
        return uxQueueMessagesWaiting(s_tx);
    }
    bool WriteBlocked() const { return write_stall_.Blocked(); }

#if !CONFIG_IDF_TARGET_ESP32C3
    void NoteHelloSent() {
        hello_sent_ms_.store(static_cast<uint32_t>(esp_timer_get_time() / 1000));
    }

    void RecordWsPoll() { ++ws_polls_; }
    void RecordWsDeliveryWait() { ++ws_delivery_waits_; }

    void RecordWsRead(int bytes, int64_t started_us, int64_t finished_us) {
        if (bytes > 0) last_ws_payload_us_ = finished_us;
        if (finished_us - started_us >= kReportAudioGapUs) {
            ESP_LOGI(TAG, "WSS slow read: start_ms=%u end_ms=%u bytes=%d",
                     static_cast<unsigned>(started_us / 1000), static_cast<unsigned>(finished_us / 1000), bytes);
        }
    }

    void RecordWsMessage() { ++ws_messages_; }

    void ReportWriteStall(const char* outcome, int64_t now_us) {
        if (!ws_stall_started_us_) return;
        const int64_t duration_us = now_us - ws_stall_started_us_;
        if (duration_us >= kReportAudioGapUs) {
            const int64_t rx_idle_us = last_ws_payload_us_ ? now_us - last_ws_payload_us_ : now_us - created_at_;
            ESP_LOGI(TAG, "WSS TX stall %s: start_ms=%u end_ms=%u duration_ms=%u rx_bytes=%u rx_messages=%u "
                          "decoded_ms=%u rx_age_ms=%u rx_polls=%u delivery_waits=%u downlink_pending_bytes=%u",
                     outcome, static_cast<unsigned>(ws_stall_started_us_ / 1000), static_cast<unsigned>(now_us / 1000),
                     static_cast<unsigned>(duration_us / 1000), static_cast<unsigned>(s_rx_bytes.load() - ws_stall_rx_bytes_),
                     ws_messages_ - ws_stall_messages_,
                     static_cast<unsigned>((s_decoded_samples.load() - ws_stall_decoded_) / 16),
                     static_cast<unsigned>(rx_idle_us / 1000), ws_polls_ - ws_stall_polls_,
                     ws_delivery_waits_ - ws_stall_delivery_waits_, static_cast<unsigned>(s_downlink_bytes.load()));
        }
        ws_stall_started_us_ = 0;
    }
#endif

    // A busy queue pauses reads, while the network loop continues to send microphone audio.
    bool Deliver(unsigned opcode, const uint8_t* data, size_t bytes, int64_t received_at, uint32_t reset) {
#if CONFIG_IDF_TARGET_ESP32C3
        received_at_ = received_at;
        if (reset != s_reset.load()) return true;
        const bool handled = opcode == WS_TRANSPORT_OPCODES_BINARY
                           ? Audio(const_cast<uint8_t*>(data), bytes) : Text(data, bytes);
        if (!handled) running_.store(false);
        return true;
#elif CONFIG_IDF_TARGET_ESP32S3
        if (s_downlink_bytes.load() + bytes > kDownlinkBytes) return false;
        void* item = nullptr;
        if (xRingbufferSendAcquire(rx_ring_, &item, sizeof(Rx) + bytes, 0) != pdTRUE) return false;
        auto* payload = static_cast<uint8_t*>(item) + sizeof(Rx);
        const Rx packet{opcode, bytes, payload, received_at, reset};
        memcpy(item, &packet, sizeof packet);
        if (bytes) memcpy(payload, data, bytes);
        s_downlink_bytes.fetch_add(bytes);
        if (xRingbufferSendComplete(rx_ring_, item) != pdTRUE) {
            running_.store(false);
            return false;
        }
        return true;
#else
        if (s_downlink_bytes.load() + bytes > kDownlinkBytes || !uxQueueSpacesAvailable(rx_queue_)) return false;
        auto* copy = static_cast<uint8_t*>(heap_caps_malloc(bytes ? bytes : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!copy) copy = static_cast<uint8_t*>(malloc(bytes ? bytes : 1));
        if (!copy) { running_.store(false); return false; }
        if (bytes) memcpy(copy, data, bytes);
        Rx packet{opcode, bytes, copy, received_at, reset};
        s_downlink_bytes.fetch_add(bytes);
        if (xQueueSend(rx_queue_, &packet, 0) == pdTRUE) return true;
        s_downlink_bytes.fetch_sub(bytes);
        free(copy);
        return false;
#endif
    }

    bool OpenDecoder() {
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        if (audio_.output_opus && !decoder_) {
            if (!decode_pcm_) {
#if AGENT_LINK_STATIC_VOICE_PSRAM
                decode_pcm_ = s_decoded_pcm;
#else
                decode_pcm_ = static_cast<int16_t*>(heap_caps_malloc(kDecodePcmBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!decode_pcm_) decode_pcm_ = static_cast<int16_t*>(malloc(kDecodePcmBytes));
                if (!decode_pcm_) return false;
#endif
            }
            esp_opus_dec_cfg_t config = {};
            config.sample_rate = 16000;
            config.channel = 1;
            config.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
            config.self_delimited = false;
            if (esp_opus_dec_open(&config, sizeof config, &decoder_) != ESP_AUDIO_ERR_OK) {
#if CONFIG_IDF_TARGET_ESP32C3
                if (codec_memory_.Reserved() && !decoder_ && !encoder_) codec_memory_.Rollback();
#endif
                ESP_LOGE(TAG, "Opus decoder could not be opened (internal free=%u largest=%u)",
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
                return false;
            }
        }
#else
        if (audio_.output_opus) return false;
#endif
        return true;
    }

    void ResetDecoder() {
        converter_.Reset();
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        if (decoder_) {
            esp_opus_dec_close(decoder_);
            decoder_ = nullptr;
#if CONFIG_IDF_TARGET_ESP32C3
            // Recreate immediately at a reply boundary, before receiving more
            // messages can consume/fragment the state block just released.
            if ((!SharedCodec() || replying_) && Healthy() && Current(epoch_) && !OpenDecoder()) running_.store(false);
#endif
        }
#endif
    }

    void ResetAudio() {
        first_audio_reported_ = false;
        if (SharedCodec()) {
            if (!EndReply()) running_.store(false);
        } else ResetDecoder();
        play_due_ = 0;
        portENTER_CRITICAL(&s_audio_reset_lock);
        reset_ = s_reset.load();
        const auto reason = s_audio_reset_reason;
        portEXIT_CRITICAL(&s_audio_reset_lock);
        if (s_cb.audio_reset) s_cb.audio_reset(reason, reset_);
    }

    void Pcm(const uint8_t* data, size_t bytes) {
        if (!s_ready || s_mute) return;
        while (bytes && Healthy() && Current(epoch_) && !s_mute && reset_ == s_reset) {
            const size_t take = bytes > 2048 ? 2048 : bytes;
            int16_t pcm[684];
            const size_t samples = converter_.Convert(data, take, pcm);
            data += take;
            bytes -= take;
            if (!samples) continue;
            Output(pcm, samples);
        }
    }

    void Output(const int16_t* pcm, size_t samples) {
        const int64_t entered = esp_timer_get_time();
        const int64_t duration = static_cast<int64_t>(samples) * 1000000 / 16000;
        const int64_t buffer = static_cast<int64_t>(agentlink::audio::BufferMs()) * 750;
        auto room = [&]() {
            size_t queued = 0;
            if (agentlink::audio::BufferLevelBytes(&queued))
                return agentlink::realtime::PlaybackRoom(queued, samples * 2, agentlink::audio::BufferMs());
            return play_due_ + duration - esp_timer_get_time() <= buffer;
        };
        if (samples * 2 > uint64_t(agentlink::audio::BufferMs()) * 32) {
            ESP_LOGE(TAG, "decoded frame exceeds playback capacity");
            running_.store(false); return;
        }
        while (!room()) {
            if (!Healthy() || !Current(epoch_) || s_mute || reset_ != s_reset) return;
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
            if (!PumpEncoder()) { running_.store(false); return; }
#endif
#if CONFIG_IDF_TARGET_ESP32C3
            if (sender_ && (!PumpWire(*sender_) || !FlushTx(*sender_, scratch_))) {
                transport_failed_ = true; running_.store(false); return;
            }
#endif
            vTaskDelay(1);
        }
        if (!Healthy() || !Current(epoch_) || s_mute || reset_ != s_reset) return;
        const int64_t before_sink = esp_timer_get_time();
        if (before_sink - entered > max_pacing_) max_pacing_ = before_sink - entered;
        if (s_cb.pcm) s_cb.pcm(reinterpret_cast<const uint8_t*>(pcm), samples * 2);
        const int64_t now = esp_timer_get_time();
        if (now - before_sink > max_sink_) max_sink_ = now - before_sink;
        if (play_due_ < now) play_due_ = now;
        play_due_ += duration;
        samples_ += samples;
        s_decoded_samples.fetch_add(samples);
    }

    bool Audio(uint8_t* data, size_t bytes) {
        if (!s_ready || s_mute || !bytes) return true;
        const int64_t now = received_at_;
        if (!first_audio_reported_) {
            first_audio_reported_ = true;
            if (s_voice_timing) s_voice_timing(AGENT_LINK_VOICE_FIRST_AUDIO, now);
#if CONFIG_IDF_TARGET_ESP32S3
            TraceCloudVoice(AGENT_LINK_VOICE_TRACE_FIRST_AUDIO, now, "response.audio.delta");
#endif
        }
        if (last_audio_) {
            const int64_t gap_us = now - last_audio_;
            if (gap_us > max_gap_) max_gap_ = gap_us;
#if !CONFIG_IDF_TARGET_ESP32C3
            if (gap_us >= kReportAudioGapUs) {
                ESP_LOGI(TAG, "WSS audio gap: reply=%u start_ms=%u end_ms=%u gap_ms=%u dispatch_ms=%u",
                         replies_, static_cast<unsigned>(last_audio_ / 1000), static_cast<unsigned>(now / 1000),
                         static_cast<unsigned>(gap_us / 1000),
                         static_cast<unsigned>((esp_timer_get_time() - now) / 1000));
            }
#endif
        }
        last_audio_ = now;
        encoded_bytes_ += bytes;
        ++packets_;
        if (!audio_.output_opus) {
            if (bytes % 2) return false;
            Pcm(data, bytes);
            return true;
        }
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        const int64_t decode_started = esp_timer_get_time();
        if (SharedCodec() && !replying_ && !BeginReply()) return false;
        if (!OpenDecoder()) return false;
        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = data;
        raw.len = static_cast<uint32_t>(bytes);
        esp_audio_dec_out_frame_t frame = {};
        frame.buffer = reinterpret_cast<uint8_t*>(decode_pcm_);
        frame.len = kDecodePcmBytes;
        esp_audio_dec_info_t info = {};
        const esp_audio_err_t result = esp_opus_dec_decode(decoder_, &raw, &frame, &info);
        const int64_t decoded_in = esp_timer_get_time() - decode_started;
        if (decoded_in > max_decode_) max_decode_ = decoded_in;
        if (result != ESP_AUDIO_ERR_OK || raw.consumed != bytes || frame.decoded_size % 2 ||
            frame.decoded_size > frame.len || info.sample_rate != 16000 || info.channel != 1) {
            ESP_LOGW(TAG, "invalid Opus packet (%d, %u bytes)", result, static_cast<unsigned>(bytes));
            return false;
        }
        Output(decode_pcm_, frame.decoded_size / 2);
        return true;
#else
        return false;
#endif
    }

    bool Text(const uint8_t* data, size_t bytes) {
#if CONFIG_IDF_TARGET_ESP32C3
        // The C3 delivers synchronously from its writable message buffer. Keep
        // Base64 there instead of making a second payload-sized cJSON string.
        uint8_t* borrowed_audio = nullptr;
        size_t audio_bytes = 0;
        if (agentlink::realtime::BorrowAudioDelta(const_cast<uint8_t*>(data), bytes, &borrowed_audio, &audio_bytes)) {
            if (s_mute || !s_ready) return true;
            size_t decoded = 0;
            return mbedtls_base64_decode(borrowed_audio, audio_bytes, &decoded, borrowed_audio, audio_bytes) == 0 &&
                   Audio(borrowed_audio, decoded);
        }
#endif
        cJSON* json = cJSON_ParseWithLength(reinterpret_cast<const char*>(data), bytes);
        if (!json) return false;
        const cJSON* type = cJSON_GetObjectItemCaseSensitive(json, "type");
        const char* event = cJSON_IsString(type) ? type->valuestring : "";
#if CONFIG_IDF_TARGET_ESP32S3
        TraceCloudControl(event, json, received_at_);
#endif
        bool keep = true;
        if (strcmp(event, "session.ready") == 0) {
            if (conversation_id_ && conversation_id_[0]) {
                const auto* connection = cJSON_GetObjectItemCaseSensitive(json, "realtime_connection");
                const auto* id = cJSON_GetObjectItemCaseSensitive(connection, "sessionId");
                if (!cJSON_IsString(id)) id = cJSON_GetObjectItemCaseSensitive(connection, "session_id");
                const bool reported = cJSON_IsString(id);
                const bool matches = reported && strcmp(conversation_id_, id->valuestring) == 0;
                ESP_LOGI(TAG, "Voice WSS conversation: session_match=%s",
                         !reported ? "not_reported" : matches ? "yes" : "no");
                if (reported && !matches) { cJSON_Delete(json); return false; }
            }
            AudioFormat format = audio_;
            if (agentlink::realtime::AudioSettings(json)) keep = ReadAudioFormat(json, &format, false);
            if (s_ready && (format.input_opus != audio_.input_opus || format.output_opus != audio_.output_opus))
                keep = false;
#if !CONFIG_AGENT_LINK_REALTIME_OPUS
            if (format.input_opus || format.output_opus) keep = false;
#endif
            if (!keep) { cJSON_Delete(json); return false; }
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
            // The first ready event may refine either direction. Release the
            // prepared state before switching allocation/half-duplex policy.
            if (format.input_opus != audio_.input_opus || format.output_opus != audio_.output_opus) {
                if (decoder_) { esp_opus_dec_close(decoder_); decoder_ = nullptr; }
                if (encoder_) { esp_opus_enc_close(encoder_); encoder_ = nullptr; }
                replying_ = false;
                ClearCodecUplink();
            }
#endif
            audio_ = format;
            input_opus_.store(audio_.input_opus);
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
            // A first ready event may refine a PCM-input grant to Opus. Free
            // the decoder from preparation before entering encoder-only mode.
            if (SharedCodec() && !replying_ && decoder_) {
                esp_opus_dec_close(decoder_);
                decoder_ = nullptr;
            }
#endif
            if (!SharedCodec() && !OpenDecoder()) { cJSON_Delete(json); return false; }
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
            // Codec allocation must succeed before enabling capture/UI readiness.
            if (audio_.input_opus && !StartEncoder()) { cJSON_Delete(json); return false; }
#endif
#if CONFIG_IDF_TARGET_ESP32S3
            ReportDmaHeap("session_ready");
#endif
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_AGENT_LINK_REALTIME_OPUS
            // Allocate the encoder and its queue while bringing up the resident socket.
            if (audio_.input_opus && !StartEncoder()) { cJSON_Delete(json); return false; }
#endif
            s_mute.store(CanPauseInput() && !(s_capture_turn.load() & 1));
            if (!s_ready.exchange(true) && s_cb.state)
                s_cb.state(true, CanPauseInput() && s_mute ? "Voice ready, microphone off" :
                                 continuous_ ? "Microphone on" : "Hold to talk, release to send");
            SetRecovering(epoch_, false);
#if !CONFIG_IDF_TARGET_ESP32C3
            const uint32_t sent_at = hello_sent_ms_.exchange(0);
            if (sent_at) {
                const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
                ESP_LOGI(TAG, "Voice stage session ready: ms=%u", static_cast<unsigned>(now - sent_at));
            }
#endif
            ESP_LOGI(TAG, "voice session ready (%s 16 kHz up, %s 24 kHz down, %s)",
                     audio_.input_opus ? "Opus" : "PCM16", audio_.output_opus ? "Opus" : "PCM16",
                     continuous_ ? "server endpointing" : "client PTT");
#if CONFIG_AGENT_LINK_REALTIME_OPUS
            if (continuous_ && (!audio_.input_opus || !audio_.output_opus))
                ESP_LOGW(TAG, "server negotiated PCM16 despite the Opus request");
#endif
        } else if (strcmp(event, "response.audio.delta") == 0) {
            const cJSON* audio = cJSON_GetObjectItemCaseSensitive(json, "audio");
            if (cJSON_IsString(audio) && !s_mute && s_ready) {
                const size_t len = strlen(audio->valuestring);
                // mbedTLS validates first, then decodes forwards (4 source
                // bytes -> 3 destination bytes). Its own string is writable;
                // decoding in place avoids a third copy of every audio delta.
                auto* decoded = reinterpret_cast<uint8_t*>(audio->valuestring);
                size_t got = 0;
                if (mbedtls_base64_decode(decoded, len, &got, decoded, len) != 0) {
                    keep = false;
                } else {
                    keep = Audio(decoded, got);
                }
            }
        } else if (CanPauseInput() && s_mute &&
                   (strncmp(event, "response.", 9) == 0 || strncmp(event, "transcript.", 11) == 0)) {
            // Keep receiving control/session events while the microphone and playback are off.
        } else if (strcmp(event, "response.audio.done") == 0) {
            ++reply_ends_;
            first_audio_reported_ = false;
#if !CONFIG_IDF_TARGET_ESP32C3
            ESP_LOGI(TAG, "WSS audio done: reply=%u packets=%u pcm_ms=%u max_gap_ms=%u max_decode_us=%u max_pacing_ms=%u",
                     replies_, packets_, static_cast<unsigned>(samples_ / 16), static_cast<unsigned>(max_gap_ / 1000),
                     static_cast<unsigned>(max_decode_), static_cast<unsigned>(max_pacing_ / 1000));
#endif
            ESP_LOGD(TAG, "reply audio end #%u (started=%u, interrupted=%u)",
                     reply_ends_, replies_, interruptions_);
            ESP_LOGD(TAG, "reply audio: %s, %u packets, %u bytes, %u ms PCM, max gap %u ms",
                     audio_.output_opus ? "Opus" : "PCM16", packets_, static_cast<unsigned>(encoded_bytes_),
                     static_cast<unsigned>(samples_ / 16), static_cast<unsigned>(max_gap_ / 1000));
            ESP_LOGD(TAG, "reply processing: decode max %u us, sink max %u us, pacing max %u ms",
                     static_cast<unsigned>(max_decode_), static_cast<unsigned>(max_sink_),
                     static_cast<unsigned>(max_pacing_ / 1000));
            if (SharedCodec()) keep = EndReply();
            else ResetDecoder();
            if (s_cb.audio_end) s_cb.audio_end();
            if (s_cb.state && s_ready)
                s_cb.state(true, continuous_ ? "Microphone on" : "Hold to talk, release to send");
        } else if (strcmp(event, "response.started") == 0) {
            ++replies_;
            first_audio_reported_ = false;
            if (s_voice_timing && s_ready && !s_mute)
                s_voice_timing(AGENT_LINK_VOICE_RESPONSE_STARTED, received_at_);
            ESP_LOGD(TAG, "reply started #%u", replies_);
            if (SharedCodec()) keep = BeginReply();
            else ResetDecoder();
            samples_ = encoded_bytes_ = packets_ = 0;
            last_audio_ = max_gap_ = 0;
            max_decode_ = max_sink_ = max_pacing_ = 0;
            if (s_cb.state && s_ready && !s_mute) s_cb.state(true, "Receiving agent reply");
        } else if (strcmp(event, "response.interrupted") == 0) {
            ++interruptions_;
            ESP_LOGD(TAG, "reply interrupted #%u (started=%u, ended=%u)", interruptions_, replies_, reply_ends_);
            if (reset_ != s_reset.load()) ResetAudio();
        } else if (strcmp(event, "task.accepted") == 0) {
            ESP_LOGI(TAG, "Realtime tool task: %s", event);
        } else if (strcmp(event, "task.result") == 0) {
            const cJSON* status = cJSON_GetObjectItemCaseSensitive(json, "status");
            const cJSON* summary = cJSON_GetObjectItemCaseSensitive(json, "summary");
            ESP_LOGI(TAG, "Realtime tool task: result=%s summary=%.220s",
                     cJSON_IsString(status) ? status->valuestring : "unknown",
                     cJSON_IsString(summary) ? summary->valuestring : "");
        } else if (strcmp(event, "transcript.partial") == 0 || strcmp(event, "transcript.final") == 0) {
            const cJSON* text = cJSON_GetObjectItemCaseSensitive(json, "text");
            if (cJSON_IsString(text)) {
                if (s_cb.text) s_cb.text(text->valuestring);
                const cJSON* role = cJSON_GetObjectItemCaseSensitive(json, "role");
                if (s_ready && Current(epoch_) && strcmp(event, "transcript.final") == 0 &&
                    cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0) {
                    if (s_voice_timing && !s_mute) s_voice_timing(AGENT_LINK_VOICE_USER_FINAL, received_at_);
                    if (s_user_transcript && conversation_id_ && conversation_id_[0]) s_user_transcript(text->valuestring);
                }
            }
        } else if (strcmp(event, "error") == 0 || strcmp(event, "session.error") == 0 ||
                   strcmp(event, "session.ended") == 0) {
            agentlink::realtime::EndDiagnostic diagnostic;
            agentlink::realtime::DescribeServerEnd(json, cloud_cred_auth_key(), connect_token_, &diagnostic);
            ESP_LOGW(TAG, "voice server terminal: event=%s code=%s reason=%s connection_age_ms=%lld retry=no",
                     diagnostic.event, diagnostic.code, diagnostic.reason,
                     static_cast<long long>((esp_timer_get_time() - created_at_) / 1000));
            keep = false;
        }
        cJSON_Delete(json);
        return keep;
    }

    bool FlushTx(WsSender& ws, TxScratch* scratch) {
        if (!s_ready) return true;
#if AGENT_LINK_NATIVE_WSS
        if (ws.pending.Busy()) return true;
#endif
#if CONFIG_AGENT_LINK_REALTIME_OPUS && !CONFIG_IDF_TARGET_ESP32C3
        if (input_opus_.load() && !StartEncoder()) return false;
#endif
        std::unique_lock<std::mutex> capture_lock(s_mutex, std::defer_lock);
        if (CanPauseInput()) {
            capture_lock.lock();
            const uint32_t turn = s_capture_turn.load();
            if (encoded_queue_) {
                while (xQueuePeek(encoded_queue_, &encoded_packet_, 0) == pdTRUE &&
                       encoded_packet_.turn != turn) {
                    if (xQueueReceive(encoded_queue_, &encoded_packet_, 0) != pdTRUE) break;
                    s_encoded_pending.fetch_sub(1);
                }
            }
            if (!(turn & 1)) return true;
        }
        if (!TxPending()) return true;
#if !AGENT_LINK_NATIVE_WSS
        // The blocking transport adapter polls before writing. Native WSS queues
        // one immutable frame and drives actual nonblocking TLS write progress.
        errno = 0;
        const int writable = PollWritable(ws.tls);
        const int saved_errno = errno;
        const int64_t now_us = esp_timer_get_time();
        const int64_t now_ms = now_us / 1000;
        write_stall_.Observe(writable > 0, now_ms);
        if (!writable && !ws_stall_started_us_) {
            ws_stall_started_us_ = now_us;
            ws_stall_rx_bytes_ = s_rx_bytes.load();
            ws_stall_messages_ = ws_messages_;
            ws_stall_decoded_ = s_decoded_samples.load();
            ws_stall_polls_ = ws_polls_;
            ws_stall_delivery_waits_ = ws_delivery_waits_;
        }
        if (writable < 0 || write_stall_.Expired(now_ms)) {
            ReportWriteStall(writable < 0 ? "error" : "timeout", now_us);
            LogTransportFailure(ws.tls, "uplink wait writable", writable, saved_errno);
            transport_failed_ = true;
            return false;
        }
        if (!writable) return true;
        ReportWriteStall("writable", now_us);
#endif
        if (input_opus_.load()) {
#if CONFIG_AGENT_LINK_REALTIME_OPUS
            if (xQueuePeek(encoded_queue_, &encoded_packet_, 0) != pdTRUE) return true;
            if (encoded_packet_.kind == TxKind::kPcm) {
                size_t wire_bytes = 0, json_bytes = 0;
                unsigned frames = 0;
                agentlink::realtime::VoiceFrameCounts voice_counts;
                const unsigned limit = continuous_ ? kOpusBatchFrames : 1;
                while (frames < limit && Current(epoch_) &&
                       xQueuePeek(encoded_queue_, &encoded_packet_, 0) == pdTRUE &&
                       encoded_packet_.kind == TxKind::kPcm) {
                    const size_t json_size = AudioJson(scratch, encoded_packet_.data, encoded_packet_.bytes);
                    if (!json_size) return false;
                    const size_t added = AppendFrame(ws, wire_bytes, scratch->json, json_size);
                    if (!added) {
                        if (!frames) return false;
                        break;  // Leave the next complete packet queued.
                    }
                    if (xQueueReceive(encoded_queue_, &encoded_packet_, 0) != pdTRUE) return false;
#if CONFIG_IDF_TARGET_ESP32S3
                    voice_counts.Add(encoded_packet_.speech);
#endif
                    s_encoded_pending.fetch_sub(1);
                    wire_bytes += added;
                    json_bytes += json_size;
                    ++frames;
                }
                batch_gate_.Reset();
                return !Current(epoch_) || WriteAudio(ws, wire_bytes, json_bytes, frames, frames, voice_counts);
            }
            if (xQueueReceive(encoded_queue_, &encoded_packet_, 0) != pdTRUE) return true;
            s_encoded_pending.fetch_sub(1);
            if (!Send(ws, encoded_packet_.kind == TxKind::kStart
                         ? "{\"type\":\"input_audio.activity\",\"kind\":\"start\"}"
                         : "{\"type\":\"input_audio.activity\",\"kind\":\"end\"}")) {
                transport_failed_ = true;
                return false;
            }
            if (encoded_packet_.kind == TxKind::kEnd) {
                if (encoded_packet_.turn == s_reset.load()) s_mute.store(false);
                if (s_cb.state) s_cb.state(true, "Waiting for the agent");
            }
            return true;
#else
            return false;
#endif
        }
        Tx packet;
        const unsigned sends_per_pass = (AGENT_LINK_NATIVE_WSS || continuous_) ? 1 : 8;
        for (unsigned i = 0; i < sends_per_pass && Current(epoch_) &&
                             xQueueReceive(s_tx, &packet, 0) == pdTRUE; ++i) {
            if (packet.kind == TxKind::kPcm) {
                size_t pcm_bytes = packet.bytes;
                unsigned pcm_packets = 1;
                agentlink::realtime::VoiceFrameCounts voice_counts;
#if CONFIG_IDF_TARGET_ESP32S3
                voice_counts.Add(DetectVoiceFrame(packet.data, packet.bytes));
#endif
                memcpy(scratch->pcm, packet.data, pcm_bytes);
                if (continuous_) {
                    Tx next;
                    while (pcm_packets < kUplinkBatchPackets && Current(epoch_) &&
                           xQueuePeek(s_tx, &next, 0) == pdTRUE && next.kind == TxKind::kPcm) {
                        if (xQueueReceive(s_tx, &next, 0) != pdTRUE) break;
#if CONFIG_IDF_TARGET_ESP32S3
                        voice_counts.Add(DetectVoiceFrame(next.data, next.bytes));
#endif
                        memcpy(scratch->pcm + pcm_bytes, next.data, next.bytes);
                        pcm_bytes += next.bytes;
                        ++pcm_packets;
                    }
                }
                if (!SendAudio(ws, scratch, scratch->pcm, pcm_bytes, pcm_packets, voice_counts)) return false;
            } else {
                if (!Send(ws, packet.kind == TxKind::kStart
                        ? "{\"type\":\"input_audio.activity\",\"kind\":\"start\"}"
                        : "{\"type\":\"input_audio.activity\",\"kind\":\"end\"}")) {
                    transport_failed_ = true;
                    return false;
                }
                if (packet.kind == TxKind::kEnd) {
                    if (packet.turn == s_reset.load()) s_mute.store(false);
                    if (s_cb.state) s_cb.state(true, "Waiting for the agent");
                }
            }
        }
        return true;
    }

private:
    bool transport_failed_ = false;  // network worker only
#if CONFIG_IDF_TARGET_ESP32C3
    WsSender* sender_ = nullptr;
    TxScratch* scratch_ = nullptr;
#else
    std::atomic<uint32_t> hello_sent_ms_{0};
    // Owned by the WSS network task. Decoded PCM is counted separately by the media task.
    int64_t last_ws_payload_us_ = 0;
    unsigned ws_messages_ = 0;
    unsigned ws_polls_ = 0;
    unsigned ws_delivery_waits_ = 0;
    int64_t ws_stall_started_us_ = 0;
    uint32_t ws_stall_rx_bytes_ = 0;
    unsigned ws_stall_messages_ = 0;
    uint32_t ws_stall_decoded_ = 0;
    unsigned ws_stall_polls_ = 0;
    unsigned ws_stall_delivery_waits_ = 0;
#endif
    agentlink::realtime::OpusBatchGate batch_gate_{kOpusBatchFrames};
    agentlink::realtime::WriteStall write_stall_;
    static void MediaEntry(void* context) {
#if AGENT_LINK_STATIC_VOICE_PSRAM
        (void)context;
        for (;;) {
            uint32_t value = 0;
            if (xTaskNotifyWait(0, UINT32_MAX, &value, portMAX_DELAY) == pdTRUE)
                reinterpret_cast<Call*>(static_cast<uintptr_t>(value))->MediaLoop();
        }
#else
        static_cast<Call*>(context)->MediaLoop();
#endif
    }

    void MediaLoop() {
        Rx packet;
        while (Healthy() && Current(epoch_)) {
            if (reset_ != s_reset) ResetAudio();
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
            if (!PumpEncoder()) { running_.store(false); break; }
#endif
            TickType_t wait = pdMS_TO_TICKS(10);
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
            if (s_ready && input_opus_.load() && uxQueueMessagesWaiting(s_tx) &&
                encoded_queue_ && uxQueueSpacesAvailable(encoded_queue_) >= 2) wait = 0;
#endif
#if CONFIG_IDF_TARGET_ESP32S3
            size_t item_size = 0;
            void* item = xRingbufferReceive(rx_ring_, &item_size, wait);
            if (!item) continue;
            memcpy(&packet, item, sizeof packet);
#else
            if (xQueueReceive(rx_queue_, &packet, wait) != pdTRUE) continue;
#endif
            received_at_ = packet.received_at;
            bool handled = true;
            if (packet.reset == s_reset.load()) {
                handled = packet.opcode == WS_TRANSPORT_OPCODES_BINARY
                        ? Audio(packet.data, packet.bytes) : Text(packet.data, packet.bytes);
            }
            s_downlink_bytes.fetch_sub(packet.bytes);
#if CONFIG_IDF_TARGET_ESP32S3
            vRingbufferReturnItem(rx_ring_, item);
#else
            free(packet.data);
#endif
            s_media_stack.store(uxTaskGetStackHighWaterMark(nullptr));
            if (!handled) running_.store(false);
        }
#if AGENT_LINK_STATIC_VOICE_PSRAM
        media_done_.store(true);
#else
        const bool caps = media_caps_;
        media_done_.store(true);
        if (caps) vTaskDeleteWithCaps(nullptr);
        else vTaskDelete(nullptr);
#endif
    }

    size_t AudioJson(TxScratch* scratch, const uint8_t* data, size_t bytes) {
        memcpy(scratch->json, kAudioPrefix, sizeof kAudioPrefix - 1);
        size_t encoded = 0;
        if (mbedtls_base64_encode(reinterpret_cast<uint8_t*>(scratch->json + sizeof kAudioPrefix - 1),
                sizeof scratch->json - sizeof kAudioPrefix - 1, &encoded, data, bytes) != 0) return 0;
        const size_t n = sizeof kAudioPrefix - 1 + encoded;
        scratch->json[n] = '"';
        scratch->json[n + 1] = '}';
        return n + 2;
    }

    bool SendAudio(WsSender& ws, TxScratch* scratch, const uint8_t* data,
                   size_t bytes, unsigned pcm_packets, agentlink::realtime::VoiceFrameCounts voice_counts) {
        const size_t json_size = AudioJson(scratch, data, bytes);
        if (!json_size) return false;
        const size_t wire_size = AppendFrame(ws, 0, scratch->json, json_size);
        return wire_size && WriteAudio(ws, wire_size, json_size, pcm_packets, 1, voice_counts);
    }

    bool WriteAudio(WsSender& ws, size_t wire_bytes, size_t json_bytes,
                    unsigned pcm_packets, unsigned messages, [[maybe_unused]] agentlink::realtime::VoiceFrameCounts voice_counts) {
#if AGENT_LINK_NATIVE_WSS
        if (!WriteWire(ws, wire_bytes)) { transport_failed_ = true; return false; }
        ws.audio_frames = pcm_packets; ws.audio_messages = messages; ws.audio_bytes = json_bytes;
        ws.audio_turn = CanPauseInput() ? s_capture_turn.load() : 0;
#if CONFIG_IDF_TARGET_ESP32S3
        ws.voice_counts = voice_counts;
#endif
        return true;
#else
        SetWorkerPhase(1);
        const int64_t started = esp_timer_get_time();
        const bool sent = WriteWire(ws, wire_bytes);
        const uint32_t elapsed_ms = static_cast<uint32_t>((esp_timer_get_time() - started) / 1000);
        s_tx_send_ms.fetch_add(elapsed_ms);
        s_tx_write_calls.fetch_add(1);
        if (elapsed_ms > s_tx_send_max_ms.load()) s_tx_send_max_ms.store(elapsed_ms);
        SetWorkerPhase(0);
        if (!sent) {
            transport_failed_ = true;
            ESP_LOGW(TAG, "audio WSS send failed after %u ms (queue=%ums)",
                     static_cast<unsigned>(elapsed_ms),
                     static_cast<unsigned>((uxQueueMessagesWaiting(s_tx) + s_encoded_pending.load()) * 20));
            return false;
        }
        const uint32_t previously_sent = s_tx_sent_frames.fetch_add(pcm_packets);
        if (continuous_ && previously_sent == 0)
            ESP_LOGI(TAG, "Voice first uplink: sent_ms=%u frames=%u",
                     static_cast<unsigned>(esp_timer_get_time() / 1000), pcm_packets);
        s_tx_sent_messages.fetch_add(messages);
        s_tx_bytes.fetch_add(json_bytes);
        return true;
#endif
    }

#if CONFIG_AGENT_LINK_REALTIME_OPUS
    bool StartEncoder() {
#if CONFIG_IDF_TARGET_ESP32C3
        if (encoder_) return !SharedCodec() || CacheSilence();
#else
        if (encoder_started_) return true;
#endif
        if (!encoded_queue_) {
#if AGENT_LINK_STATIC_VOICE_PSRAM
            encoded_queue_ = xQueueCreateStatic(kEncodedDepth, sizeof(EncodedTx),
                                               reinterpret_cast<uint8_t*>(s_encoded_storage), &s_encoded_control);
#else
            encoded_queue_ = xQueueCreateWithCaps(kEncodedDepth, sizeof(EncodedTx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            encoded_queue_caps_ = encoded_queue_ != nullptr;
            if (!encoded_queue_) encoded_queue_ = xQueueCreate(kEncodedDepth, sizeof(EncodedTx));
#endif
        }
        if (!encoded_queue_) return false;
#if CONFIG_IDF_TARGET_ESP32C3
        if (SharedCodec() && replying_) return silence_ != nullptr;
        const bool opened = OpenEncoder() && (!SharedCodec() || CacheSilence());
        ESP_LOGD(TAG, "shared Opus worker: codec=%s queue=%u; internal free=%u largest=%u",
                 opened ? "ready" : "failed", static_cast<unsigned>(kEncodedDepth),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        return opened;
#else
#if AGENT_LINK_STATIC_VOICE_PSRAM
        EXT_RAM_BSS_ATTR alignas(16) static StackType_t encoder_stack[40960];
        static StaticTask_t encoder_tcb;
        static const TaskHandle_t encoder = xTaskCreateStatic(&EncoderEntry, "agentlink_encode", sizeof(encoder_stack),
                                                               nullptr, 5, encoder_stack, &encoder_tcb);
        encoder_started_ = DispatchAudioWorker(encoder, this);
        encoder_caps_ = true;
#else
        encoder_caps_ = true;
        encoder_started_ = xTaskCreateWithCaps(&EncoderEntry, "agentlink_encode", 40960, this, 5,
                                               nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
        if (!encoder_started_) {
            encoder_caps_ = false;
            encoder_started_ = xTaskCreate(&EncoderEntry, "agentlink_encode", 40960, this, 5, nullptr) == pdPASS;
        }
#endif
        if (!encoder_started_) ESP_LOGE(TAG, "could not start microphone Opus worker");
        else ESP_LOGD(TAG, "Opus encoder stack=%s; internal free=%u largest=%u",
                      encoder_caps_ ? "PSRAM" : "internal",
                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        return encoder_started_;
#endif
    }

    static void EncoderEntry(void* context) {
#if AGENT_LINK_STATIC_VOICE_PSRAM
        (void)context;
        for (;;) {
            uint32_t value = 0;
            if (xTaskNotifyWait(0, UINT32_MAX, &value, portMAX_DELAY) == pdTRUE)
                reinterpret_cast<Call*>(static_cast<uintptr_t>(value))->EncoderLoop();
        }
#else
        static_cast<Call*>(context)->EncoderLoop();
#endif
    }

    bool QueueEncoded(const EncodedTx& packet) {
        while (Healthy() && Current(epoch_)) {
            if (CanPauseInput() && packet.turn != s_capture_turn.load()) return true;
            s_encoded_pending.fetch_add(1);
#if CONFIG_IDF_TARGET_ESP32C3
            // PumpEncoder reserves enough slots for the frame and end marker.
            if (xQueueSend(encoded_queue_, &packet, 0) == pdTRUE) return true;
            s_encoded_pending.fetch_sub(1);
            return false;
#else
            if (xQueueSend(encoded_queue_, &packet, pdMS_TO_TICKS(10)) == pdTRUE) return true;
            s_encoded_pending.fetch_sub(1);
#endif
        }
        return false;
    }

    bool Encode(const uint8_t* pcm, EncodedTx* packet) {
#if CONFIG_IDF_TARGET_ESP32S3
        packet->speech = DetectVoiceFrame(pcm, kMicBytes);
#endif
        if (SharedCodec() && replying_) {
            // Preserve endpointing cadence while the application is half-duplex.
            // Reusing a valid encoded silence packet needs no live encoder state.
            if (!silence_ || !silence_bytes_) return false;
            memcpy(packet->data, silence_, silence_bytes_);
            packet->kind = TxKind::kPcm;
            packet->bytes = silence_bytes_;
            return QueueEncoded(*packet);
        }
        esp_audio_enc_in_frame_t input{const_cast<uint8_t*>(pcm), kMicBytes};
        esp_audio_enc_out_frame_t output{};
        output.buffer = packet->data;
        output.len = sizeof packet->data;
        const int64_t started = esp_timer_get_time();
        const esp_audio_err_t result = esp_opus_enc_process(encoder_, &input, &output);
        const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - started);
        if (elapsed > s_tx_encode_max_us.load()) s_tx_encode_max_us.store(elapsed);
        if (result != ESP_AUDIO_ERR_OK || !output.encoded_bytes || output.encoded_bytes > output.len) {
            ESP_LOGW(TAG, "microphone Opus encoding failed (%d)", result);
            return false;
        }
        packet->kind = TxKind::kPcm;
        packet->bytes = static_cast<uint16_t>(output.encoded_bytes);
        return QueueEncoded(*packet);
    }

    bool OpenEncoder() {
#if CONFIG_IDF_TARGET_ESP32C3
        if (SharedCodec() && !codec_memory_.Reserve()) return false;
#endif
        esp_opus_enc_config_t config = ESP_OPUS_ENC_CONFIG_DEFAULT();
        config.sample_rate = 16000;
        config.channel = 1;
        config.bitrate = 24000;
        config.enable_vbr = true;
        config.complexity = 0;
        int input_size = 0, output_size = 0;
        const bool valid = esp_opus_enc_open(&config, sizeof config, &encoder_) == ESP_AUDIO_ERR_OK &&
                     esp_opus_enc_get_frame_size(encoder_, &input_size, &output_size) == ESP_AUDIO_ERR_OK &&
                     input_size == kMicBytes && output_size <= sizeof(EncodedTx::data);
        if (!valid) ESP_LOGE(TAG, "Opus encoder initialization failed (input=%d output=%d)", input_size, output_size);
#if CONFIG_IDF_TARGET_ESP32C3
        if (!valid && codec_memory_.Reserved() && !encoder_ && !decoder_) codec_memory_.Rollback();
#endif
        return valid;
    }

    bool CacheSilence() {
#if CONFIG_IDF_TARGET_ESP32C3
        if (silence_) return true;
        uint8_t pcm[kMicBytes]{};
        esp_audio_enc_in_frame_t input{pcm, sizeof pcm};
        esp_audio_enc_out_frame_t output{};
        output.buffer = encoded_packet_.data;
        output.len = sizeof encoded_packet_.data;
        if (esp_opus_enc_process(encoder_, &input, &output) != ESP_AUDIO_ERR_OK ||
            !output.encoded_bytes || output.encoded_bytes > output.len) return false;
        silence_ = static_cast<uint8_t*>(malloc(output.encoded_bytes));
        if (!silence_) return false;
        silence_bytes_ = static_cast<uint16_t>(output.encoded_bytes);
        memcpy(silence_, output.buffer, silence_bytes_);
#endif
        return true;
    }

#if CONFIG_IDF_TARGET_ESP32C3
    bool PumpEncoder() {
        if (!s_ready || !input_opus_.load()) return true;
        // An end marker can flush a padded frame plus a control packet. Never
        // wait for network space here: decoding must continue during TX stalls.
        if (!encoded_queue_) return false;
        Tx input;
        if (xQueuePeek(s_tx, &input, 0) != pdTRUE) return true;
        const unsigned needed = input.kind == TxKind::kEnd ? 2 : 1;
        if (uxQueueSpacesAvailable(encoded_queue_) < needed) return true;
        if (xQueueReceive(s_tx, &input, 0) != pdTRUE) return true;
        EncodedTx output{};
        const bool valid = EncodeInput(input, shared_frames_, output);
        s_encoder_stack.store(uxTaskGetStackHighWaterMark(nullptr));
        return valid;
    }
    agentlink::realtime::PcmFrames<kMicBytes> shared_frames_;
#endif

    bool EncodeInput(const Tx& input, agentlink::realtime::PcmFrames<kMicBytes>& frames, EncodedTx& output) {
        output.turn = input.turn;
        if (input.kind == TxKind::kPcm)
            return frames.Append(input.data, input.bytes, [&](const uint8_t* pcm) { return Encode(pcm, &output); });
        bool valid = true;
        if (input.kind == TxKind::kEnd)
            valid = frames.Flush([&](const uint8_t* pcm) { return Encode(pcm, &output); });
        frames.Reset();
        output.kind = input.kind;
        output.bytes = 0;
        return valid && QueueEncoded(output);
    }

    void EncoderLoop() {
        bool valid = OpenEncoder();
        agentlink::realtime::PcmFrames<kMicBytes> frames;
        Tx input;
        EncodedTx output{};
        uint32_t turn = 0;
        int64_t last_yield = esp_timer_get_time();
        while (valid && Healthy() && Current(epoch_)) {
            if (xQueueReceive(s_tx, &input, pdMS_TO_TICKS(10)) != pdTRUE) continue;
            if (CanPauseInput()) {
                if (input.turn != s_capture_turn.load()) continue;
                if (input.turn != turn) {
                    frames.Reset();
                    turn = input.turn;
                }
            }
            valid = EncodeInput(input, frames, output);
            s_encoder_stack.store(uxTaskGetStackHighWaterMark(nullptr));
            if (esp_timer_get_time() - last_yield >= kWorkerYieldUs) {
                vTaskDelay(1);
                last_yield = esp_timer_get_time();
            }
        }
        if (!valid && Healthy() && Current(epoch_)) {
            ESP_LOGW(TAG, "microphone Opus worker stopped after a codec error");
            running_.store(false);
        }
#if AGENT_LINK_STATIC_VOICE_PSRAM
        encoder_done_.store(true);
#else
        const bool caps = encoder_caps_;
        encoder_done_.store(true);
        if (caps) vTaskDeleteWithCaps(nullptr);
        else vTaskDelete(nullptr);
#endif
    }
#endif

    uint32_t epoch_;
    bool continuous_ = false;
    bool replying_ = false;
    uint8_t* silence_ = nullptr;
    uint16_t silence_bytes_ = 0;
    unsigned replies_ = 0, reply_ends_ = 0, interruptions_ = 0;
    uint32_t reset_ = s_reset.load();
    int64_t play_due_ = 0;
    AudioFormat audio_;
    std::atomic<bool> input_opus_{false};
    QueueHandle_t encoded_queue_ = nullptr;
    bool encoded_queue_caps_ = false;
    EncodedTx encoded_packet_{};
    bool encoder_started_ = false, encoder_caps_ = false;
    std::atomic<bool> encoder_done_{false};
#if CONFIG_IDF_TARGET_ESP32S3
    RingbufHandle_t rx_ring_ = nullptr;
#else
    QueueHandle_t rx_queue_ = nullptr;
    bool rx_caps_ = false;
#endif
    bool media_started_ = false, media_caps_ = false;
    std::atomic<bool> running_{true}, media_done_{false};
    int16_t* decode_pcm_ = nullptr;
    const char* connect_token_ = nullptr;
    const char* conversation_id_ = nullptr;
    const int64_t created_at_;
    int64_t received_at_ = 0;
    bool first_audio_reported_ = false;
    size_t samples_ = 0, encoded_bytes_ = 0;
    unsigned packets_ = 0;
    int64_t last_audio_ = 0, max_gap_ = 0;
    int64_t max_decode_ = 0, max_sink_ = 0, max_pacing_ = 0;
#if CONFIG_AGENT_LINK_REALTIME_OPUS
    void* decoder_ = nullptr;
    void* encoder_ = nullptr;
#endif
    agentlink::realtime::Pcm24To16 converter_;
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
    // Destroyed after the destructor body has closed both vendor handles.
    agentlink::realtime::CodecWorkspace codec_memory_;
#endif
};

#if AGENT_LINK_NATIVE_WSS
class NativeWss {
public:
    NativeWss() : tls(esp_tls_init()) {}
    ~NativeWss() { if (tls) esp_tls_conn_destroy(tls); }
    bool Connect(const WssAddress& addr, uint32_t epoch, TxScratch& scratch,
                 agentlink::realtime::MessageBuffer& storage, size_t* trailing, int* status) {
        *trailing = 0; *status = 0;
        if (!tls) return false;
        esp_tls_cfg_t config{};
        config.non_block = true;
        config.timeout_ms = kConnectTimeoutMs;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        const char* connect_host = addr.host;
#if CONFIG_IDF_TARGET_ESP32S3
        // Offer TLS 1.3 and TLS 1.2 in the same ClientHello. The peer can
        // select 1.2 without a second TCP connection or a failed handshake.
        config.tls_version = ESP_TLS_VER_ANY;
        char cached_ip[IPADDR_STRLEN_MAX];
        const bool cached_dns = CopyVoiceDns(addr.host, cached_ip);
        if (cached_dns) {
            connect_host = cached_ip;
            dns_path = "cached_ip";
        }
        // Address selection is independent of certificate verification and SNI.
        config.common_name = addr.host;
#endif
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
        config.client_session = s_wss_ticket.For(addr);
        ticket_offered = config.client_session != nullptr;
#endif
#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
        // Reserve the peer's full record after the handshake, before Opus.
        // Dynamic TX remains small; RX is reused for the life of this call.
        config.esp_tls_dyn_buf_strategy = ESP_TLS_DYN_BUF_RX_STATIC;
#endif
        const int64_t deadline = esp_timer_get_time() + kConnectTimeoutMs * 1000LL;
        const auto handshake = [&] {
            const int64_t setup_started = esp_timer_get_time();
            setup_ms = -1;
            while (Current(epoch) && esp_timer_get_time() < deadline) {
                const int result = esp_tls_conn_new_async(connect_host, strlen(connect_host), addr.port, &config, tls);
                if (setup_ms < 0) setup_ms = static_cast<int>((esp_timer_get_time() - setup_started) / 1000);
                if (result != 0) return result == 1 && Current(epoch);
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
                // ESP-IDF reapplies client_session on every handshake step.
                // mbedTLS accepts it only before processing the ClientHello.
                esp_tls_conn_state_t state;
                if (config.client_session && esp_tls_get_conn_state(tls, &state) == ESP_OK &&
                    state == ESP_TLS_HANDSHAKE) config.client_session = nullptr;
#endif
                vTaskDelay(1);
            }
            return false;
        };
        if (!handshake()) {
#if CONFIG_IDF_TARGET_ESP32S3
            if (!Current(epoch) || (!cached_dns && !ticket_offered)) return false;
            LogTransportFailure(tls, "TLS initial attempt", -1, errno);
            // No session.connect has been sent. The grant can still be used
            // immediately on the fresh TLS socket.
            if (cached_dns) {
                cloud_realtime_invalidate_dns();
                dns_path = "resolver_fallback";
                connect_host = addr.host;
            }
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
            if (ticket_offered) s_wss_ticket.session.reset();
            config.client_session = nullptr;
#endif
            ticket_fallback = ticket_offered;
            esp_tls_conn_destroy(tls);
            tls = esp_tls_init();
            if (!tls || !handshake()) return false;
#else
            return false;
#endif
        }
        tls_ready_us = esp_timer_get_time();
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_ESP_TLS_USING_MBEDTLS
        if (const auto* ssl = static_cast<const mbedtls_ssl_context*>(esp_tls_get_ssl_context(tls))) {
            tls_version = mbedtls_ssl_get_version(ssl);
            if (const char* cipher = mbedtls_ssl_get_ciphersuite(ssl)) tls_cipher = cipher;
        }
#endif
        int socket = -1;
        const int enabled = 1;
        if (esp_tls_get_conn_sockfd(tls, &socket) != ESP_OK || socket < 0) return false;
        (void)setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof enabled);

        uint8_t nonce[16], digest[20];
        char key[25]{}, challenge[80]{}, accept[29]{};
        size_t encoded = 0;
        esp_fill_random(nonce, sizeof nonce);
        if (mbedtls_base64_encode(reinterpret_cast<uint8_t*>(key), sizeof key, &encoded,
                                  nonce, sizeof nonce) != 0) return false;
        const int challenge_size = snprintf(challenge, sizeof challenge, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
        if (challenge_size <= 0 || challenge_size >= static_cast<int>(sizeof challenge)) return false;
#if MBEDTLS_VERSION_MAJOR >= 4
        size_t hashed = 0;
        const bool hash_ok = psa_hash_compute(PSA_ALG_SHA_1,
            reinterpret_cast<const uint8_t*>(challenge), challenge_size, digest, sizeof digest, &hashed) == PSA_SUCCESS && hashed == sizeof digest;
#else
        const bool hash_ok = mbedtls_sha1(reinterpret_cast<const uint8_t*>(challenge), challenge_size, digest) == 0;
#endif
        if (!hash_ok ||
            mbedtls_base64_encode(reinterpret_cast<uint8_t*>(accept), sizeof accept, &encoded,
                                  digest, sizeof digest) != 0) return false;
        const int request_size = snprintf(scratch.json, sizeof scratch.json,
            "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Protocol: tidb-link.realtime.v1\r\n\r\n", addr.path, addr.host, addr.port, key);
        if (request_size <= 0 || request_size >= static_cast<int>(sizeof scratch.json)) return false;
        int result = 0;
        size_t sent = 0, received = 0;
        auto* header = reinterpret_cast<char*>(storage.Data());
        while (Current(epoch) && esp_timer_get_time() < deadline) {
            if (sent < static_cast<size_t>(request_size)) {
                result = esp_tls_conn_write(tls, scratch.json + sent, request_size - sent);
                if (result > 0) sent += result;
                else if (!TlsWouldBlock(result)) return false;
            } else {
                if (received >= storage.Capacity() - 1) return false;
                result = esp_tls_conn_read(tls, header + received, storage.Capacity() - received - 1);
                if (result > 0) {
                    received += result; header[received] = '\0';
                    const char* delimiter = strstr(header, "\r\n\r\n");
                    if (delimiter) {
                        const size_t header_size = delimiter + 4 - header;
                        const char saved = header[header_size];
                        header[header_size] = '\0';
                        const bool valid = agentlink::realtime::ValidUpgrade(header, accept, status);
                        header[header_size] = saved;
                        if (!valid || received - header_size > sizeof scratch.read) return false;
                        *trailing = received - header_size;
                        memcpy(scratch.read, header + header_size, *trailing);
                        return true;
                    }
                } else if (!TlsWouldBlock(result)) return false;
            }
            vTaskDelay(1);
        }
        return false;
    }
    esp_tls_t* tls;
    int64_t tls_ready_us = 0;
    int setup_ms = -1;
    const char* tls_version = "-";
    const char* tls_cipher = "-";
    const char* dns_path = "resolver";
    bool ticket_offered = false;
    bool ticket_fallback = false;
};

// One worker owns the nonblocking TLS socket. S3 uses separate audio workers;
// C3 also runs the codecs here to fit its smaller RAM budget.
bool ConnectOnce(const Grant& grant, agentlink::realtime::RetryBudget& recovery) {
    WssAddress addr{};
    if (!ParseWss(grant.url, &addr)) return false;
    agentlink::cloud::NetworkMemoryLock memory;
    if (!Current(grant.epoch)) return false;
    VoiceWifiPower power;
#if CONFIG_IDF_TARGET_ESP32S3
    ReportDmaHeap("before_wss");
#endif
#if AGENT_LINK_STATIC_VOICE_PSRAM
    agentlink::realtime::MessageBuffer message(s_ws_message, sizeof(s_ws_message));
    auto* scratch = &s_ws_scratch;
#else
    agentlink::realtime::MessageBuffer message(kMessageInitialBytes, kMessageBytes, ResizeMessage);
    std::unique_ptr<TxScratch, decltype(&free)> scratch_owner(
        static_cast<TxScratch*>(heap_caps_malloc(sizeof(TxScratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), &free);
    if (!scratch_owner) scratch_owner.reset(static_cast<TxScratch*>(malloc(sizeof(TxScratch))));
    auto* scratch = scratch_owner.get();
#endif
    // Keep metadata off the shared stack: the codec needs most of its depth.
#if AGENT_LINK_STATIC_VOICE_PSRAM
    EXT_RAM_BSS_ATTR alignas(Call) static uint8_t call_storage[sizeof(Call)];
    std::unique_ptr<Call, void (*)(Call*)> owner(
        new (call_storage) Call(grant.epoch, grant.audio, grant.platform, grant.token,
                                grant.platform ? grant.session_id : nullptr),
        [](Call* call) { call->~Call(); });
#else
    std::unique_ptr<Call> owner(new (std::nothrow) Call(grant.epoch, grant.audio, grant.platform, grant.token,
                                                      grant.platform ? grant.session_id : nullptr));
#endif
    if (!message.Data() || !scratch || !owner) return false;
    NativeWss connection;
    size_t trailing = 0;
    int status = 0;
    if (s_cb.state) s_cb.state(false, "Connecting voice");
    const int64_t handshake_started = esp_timer_get_time();
    const bool connected = connection.Connect(addr, grant.epoch, *scratch, message, &trailing, &status);
    const int64_t handshake_finished = esp_timer_get_time();
    const int tls_ms = connection.tls_ready_us ? static_cast<int>((connection.tls_ready_us - handshake_started) / 1000) : -1;
    const int upgrade_ms = connection.tls_ready_us ? static_cast<int>((handshake_finished - connection.tls_ready_us) / 1000) : -1;
    ESP_LOGI(TAG, "Voice stage WSS handshake: ms=%u tls=%s cipher=%s tls_ms=%d setup_ms=%d upgrade_ms=%d dns=%s ticket=%s end_ms=%u result=%d (nonblocking)",
             static_cast<unsigned>((handshake_finished - handshake_started) / 1000),
             connection.tls_version, connection.tls_cipher, tls_ms, connection.setup_ms,
             upgrade_ms, connection.dns_path,
             connection.ticket_fallback ? "fallback" : connection.ticket_offered ? "offered" : "none",
             static_cast<unsigned>(handshake_finished / 1000),
             connected ? 0 : -1);
    if (!connected) {
        LogTransportFailure(connection.tls, "connect/upgrade", -1, errno);
        ESP_LOGW(TAG, "WSS connection failed (upgrade HTTP %d)", status);
        return status <= 0 || status >= 500;
    }
#if CONFIG_IDF_TARGET_ESP32S3
    ReportDmaHeap("after_wss");
#endif
    // Keep a full message alongside the fixed TLS record. Growing/shrinking
    // this buffer during playback otherwise introduces another large realloc.
    if (!message.Reserve(kMessageBytes)) {
        ESP_LOGW(TAG, "could not reserve %u-byte voice message", static_cast<unsigned>(kMessageBytes));
        return false;
    }
    ESP_LOGD(TAG, "WSS connected on resident stack: internal free=%u largest=%u stack_free=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    ESP_LOGD(TAG, "voice receive buffers: TLS RX=fixed, message=%uB retained", static_cast<unsigned>(message.Capacity()));
    Call& call = *owner;
    if (!Current(grant.epoch) || !call.Prepare() || !call.Start()) return false;
    WsSender sender{connection.tls, scratch->wire, sizeof scratch->wire};
#if CONFIG_IDF_TARGET_ESP32C3
    call.BindNetwork(&sender, scratch);
#endif
    char hello[sizeof Grant::token * 2 + 128];
    size_t n = snprintf(hello, sizeof hello, "{\"type\":\"session.connect\",\"connect_token\":\"");
    for (const char* p = grant.token; *p && n + 4 < sizeof hello; ++p) {
        if (*p == '"' || *p == '\\') hello[n++] = '\\';
        hello[n++] = *p;
    }
    n += snprintf(hello + n, sizeof hello - n, "\",\"endpointing\":{\"mode\":\"%s\"}}",
                  grant.platform ? "server" : "client");
    const bool queued = Send(sender, hello, n);
    memset(hello, 0, sizeof hello);
    if (!queued) return true;
    bool hello_sent = false;

    agentlink::realtime::FrameReader reader(kMessageBytes);
    agentlink::realtime::Message assembler(message.Data(), message.Capacity());
    uint8_t control[125]{}, pong[125]{};
    size_t control_size = 0, pong_size = 0;
    bool pong_pending = false, retryable = false, pending_message = false;
    uint32_t pending_reset = 0;
    int64_t pending_at = 0;
    const int64_t connected_at = esp_timer_get_time();
    int64_t last_rx = connected_at, last_ping = connected_at, last_yield = connected_at;
    uint32_t observed_tx = s_tx_sent_frames.load();
    auto deliver = [&](unsigned opcode) {
        pending_message = !call.Deliver(opcode, message.Data(), assembler.Size(), pending_at, pending_reset);
    };
    auto receive = [&](const uint8_t* data, size_t bytes, size_t* consumed) {
        return reader.Consume(data, bytes, [&](unsigned opcode, bool first, bool end, bool fin,
                                              const uint8_t* payload, size_t count) {
            if (opcode >= 8) {
                if (first) control_size = 0;
                if (count > sizeof control - control_size) return false;
                memcpy(control + control_size, payload, count); control_size += count;
                if (!end) return true;
                if (opcode == 8) {
                    const unsigned code = control_size >= 2 ? (unsigned(control[0]) << 8) | control[1] : 0;
                    ESP_LOGW(TAG, "voice peer closed (code=%u); automatic retry disabled", code);
                    return false;
                }
                if (opcode == 9) {
                    memcpy(pong, control, control_size); pong_size = control_size; pong_pending = true;
                }
                return true;
            }
            const size_t buffered = assembler.Active() ? assembler.Size() : 0;
            if (!message.Reserve(buffered + count)) return false;
            assembler.Rebind(message.Data(), message.Capacity());
            const auto result = assembler.Append(opcode, first, end, fin, payload, count);
            if (result == agentlink::realtime::Message::kError) return false;
            if (result == agentlink::realtime::Message::kComplete) {
                if (assembler.Opcode() == 1)
                    CheckResponseInterruption(message.Data(), assembler.Size(), last_rx, grant.token);
                pending_at = last_rx;
#if CONFIG_IDF_TARGET_ESP32S3
                sender.last_data_received_ms = pending_at / 1000;
#endif
                pending_reset = s_reset.load();
                deliver(assembler.Opcode());
            }
            return call.Healthy() && Current(grant.epoch);
        }, &pending_message, consumed);
    };
    // Preserve bytes received with the HTTP upgrade (including an early ping).
    size_t rx_offset = 0, rx_size = trailing;
    bool valid = true;
    while (valid && Current(grant.epoch) && call.Healthy()) {
#if CONFIG_IDF_TARGET_ESP32C3
        if (!call.Pump()) { retryable = call.TransportFailed(); break; }
#endif
        if (!PumpWire(sender)) { retryable = true; break; }
        const int64_t now = esp_timer_get_time();
        if (!hello_sent && !sender.pending.Busy()) {
            hello_sent = true;
#if !CONFIG_IDF_TARGET_ESP32C3
            call.NoteHelloSent();
#endif
            ESP_LOGI(TAG, "Voice stage session.connect sent: after_upgrade_ms=%u",
                     static_cast<unsigned>((now - handshake_finished) / 1000));
        }
        // Reply to server pings and send our keepalive before queuing more audio.
        if (!PumpControl(sender, pong, pong_size, &pong_pending, &last_ping, now)) {
            retryable = true; break;
        }
        if (hello_sent && !call.FlushTx(sender, scratch)) { retryable = call.TransportFailed(); break; }
        if ((!s_ready && now - connected_at > kReadyTimeoutUs) || now - last_rx > 45000000) {
            ESP_LOGW(TAG, "voice session timed out"); retryable = true; break;
        }
        if (pending_message) deliver(assembler.Opcode());
        int got = 0;
        if (!pending_message) {
            if (rx_offset == rx_size) {
                SetWorkerPhase(3);
                errno = 0;
                got = esp_tls_conn_read(connection.tls, scratch->read, sizeof scratch->read);
                SetWorkerPhase(0);
                if (got > 0) {
                    s_rx_bytes.fetch_add(got);
                    last_rx = esp_timer_get_time();
                    rx_offset = 0;
                    rx_size = static_cast<size_t>(got);
                } else if (!TlsWouldBlock(got)) {
                    LogTransportFailure(connection.tls, "read", got, errno); retryable = true; break;
                }
            }
            if (rx_offset < rx_size) {
                size_t consumed = 0;
                valid = receive(scratch->read + rx_offset, rx_size - rx_offset, &consumed);
                rx_offset += consumed;
                if (!valid) retryable = call.TransportFailed();
            }
        }
        const uint32_t sent_frames = s_tx_sent_frames.load();
        if (recovery.Observe(s_ready.load(), sent_frames != observed_tx, got > 0,
                             esp_timer_get_time() / 1000))
            ESP_LOGI(TAG, "voice retry budget reset: READY and bidirectional progress stable for 120s");
        observed_tx = sent_frames;
        // Yield even under a burst of replies so I2S and the idle task run.
        if (got <= 0 || esp_timer_get_time() - last_yield >= 10000) {
            vTaskDelay(1); last_yield = esp_timer_get_time();
        }
    }
    call.Stop();
    // Do not append a close/control message behind an incomplete TLS write.
    if (!retryable && !sender.pending.Busy()) {
        if (Send(sender, "{\"type\":\"session.end\"}")) (void)PumpWire(sender);
    }
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    s_wss_ticket.Save(addr, connection.tls);
#endif
    memset(scratch->wire, 0, sizeof scratch->wire);
    return retryable;
}
#else
// True only for transport failures eligible for a new platform grant.
bool ConnectOnce(const Grant& grant, agentlink::realtime::RetryBudget& recovery) {
    bool retryable = false;
    WssAddress addr = {};
    if (!ParseWss(grant.url, &addr)) {
        ESP_LOGW(TAG, "rejected an invalid WSS URL");
        return false;
    }

    // Keep background HTTPS out until the codec states, task and TLS are freed.
    agentlink::cloud::NetworkMemoryLock memory;
    if (!Current(grant.epoch)) return false;
    VoiceWifiPower power;
    esp_transport_handle_t tls = esp_transport_ssl_init();
    if (!tls) {
        ESP_LOGE(TAG, "could not allocate TLS transport");
        return false;
    }
    esp_transport_ssl_crt_bundle_attach(tls, esp_crt_bundle_attach);
    esp_transport_handle_t ws = esp_transport_ws_init(tls);
    if (!ws) {
        ESP_LOGE(TAG, "could not allocate WebSocket transport");
        esp_transport_destroy(tls);
        return false;
    }
    const esp_transport_ws_config_t config = {
        .ws_path = addr.path,
        .sub_protocol = "tidb-link.realtime.v1",
        .propagate_control_frames = true,
    };
    if (esp_transport_ws_set_config(ws, &config) != ESP_OK) {
        ESP_LOGE(TAG, "could not set the realtime subprotocol");
        esp_transport_destroy(ws);
        esp_transport_destroy(tls);
        return false;
    }

    agentlink::realtime::MessageBuffer message(kMessageInitialBytes, kMessageBytes, ResizeMessage);
    if (!message.Data()) {
        esp_transport_destroy(ws);
        esp_transport_destroy(tls);
        return false;
    }
    auto* scratch = static_cast<TxScratch*>(heap_caps_malloc(sizeof(TxScratch),
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!scratch) scratch = static_cast<TxScratch*>(malloc(sizeof(TxScratch)));
    if (!scratch) {
        ESP_LOGE(TAG, "could not allocate voice uplink buffer");
        esp_transport_destroy(ws);
        esp_transport_destroy(tls);
        return false;
    }
    if (s_cb.state) s_cb.state(false, "Connecting voice");
    Call call(grant.epoch, grant.audio, grant.platform, grant.token, grant.platform ? grant.session_id : nullptr);
    // Reserve large codec states early, but leave the 40 KB C3 task stack free
    // for certificate parsing and RSA verification during the TLS handshake.
    if (!call.Prepare()) {
        free(scratch);
        esp_transport_destroy(ws);
        esp_transport_destroy(tls);
        return false;
    }
    WsSender sender{tls, scratch->wire, sizeof scratch->wire};
    const int64_t handshake_started = esp_timer_get_time();
    errno = 0;
    const int connected = esp_transport_connect(ws, addr.host, addr.port, kConnectTimeoutMs);
    const int saved_errno = errno;
    const int64_t handshake_finished = esp_timer_get_time();
    ESP_LOGI(TAG, "Voice stage WSS handshake: ms=%u result=%d",
             static_cast<unsigned>((handshake_finished - handshake_started) / 1000), connected);
    if (connected != 0) {
        const int status = esp_transport_ws_get_upgrade_request_status(ws);
        LogTransportFailure(tls, "connect", connected, saved_errno);
        ESP_LOGW(TAG, "WSS connection failed (upgrade HTTP %d)", status);
        retryable = status <= 0 || status >= 500;
    } else {
        ESP_LOGD(TAG, "WSS connected before audio task: internal free=%u largest=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        const int socket = esp_transport_get_socket(tls);
        const int enabled = 1;
        if (socket < 0 || setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof enabled) != 0)
            ESP_LOGW(TAG, "could not enable TCP_NODELAY for voice");
        char hello[sizeof Grant::token * 2 + 128];
        size_t n = static_cast<size_t>(snprintf(hello, sizeof hello,
                                               "{\"type\":\"session.connect\",\"connect_token\":\""));
        for (const char* p = grant.token; *p && n + 4 < sizeof hello; ++p) {
            if (*p == '"' || *p == '\\') hello[n++] = '\\';
            hello[n++] = *p;
        }
        n += static_cast<size_t>(snprintf(hello + n, sizeof hello - n,
                "\",\"endpointing\":{\"mode\":\"%s\"}}", grant.platform ? "server" : "client"));
        if (!Current(grant.epoch)) {
            ESP_LOGI(TAG, "voice connection cancelled before session.connect");
        } else if (!call.Start()) {
            ESP_LOGE(TAG, "voice audio startup failed after TLS handshake");
        } else if (!Send(sender, hello, n)) {
            ESP_LOGW(TAG, "session.connect could not be sent");
            retryable = true;
        } else {
            call.NoteHelloSent();
            ESP_LOGI(TAG, "session.connect sent over WSS");
            ESP_LOGI(TAG, "Voice stage session.connect: ms=%u",
                     static_cast<unsigned>((esp_timer_get_time() - handshake_finished) / 1000));
            agentlink::realtime::Message assembler(message.Data(), message.Capacity());
            uint8_t* chunk = scratch->read;
            int frame_read = 0;
            bool pending = false;
            int64_t pending_at = 0;
            uint32_t pending_reset = 0;
            const int64_t connected_at = esp_timer_get_time();
            int64_t last_rx = connected_at, last_ping = connected_at;
            int64_t last_yield = connected_at;
            uint32_t observed_tx = s_tx_sent_frames.load();
            SetWorkerPhase(0);
            while (Current(grant.epoch) && call.Healthy()) {
                // A backlog can keep both TLS writes and zero-time polls immediately runnable.
                if (esp_timer_get_time() - last_yield >= kWorkerYieldUs) {
                    vTaskDelay(1);
                    last_yield = esp_timer_get_time();
                }
                if (!call.FlushTx(sender, scratch)) {
                    retryable = call.TransportFailed();
                    break;
                }
                if (pending) {
                    if (!call.Deliver(assembler.Opcode(), message.Data(), assembler.Size(), pending_at, pending_reset)) {
                        call.RecordWsDeliveryWait();
                        vTaskDelay(1);
                        continue;
                    }
                    pending = false;
                    message.Trim();
                    assembler.Rebind(message.Data(), message.Capacity());
                }
                const int64_t now = esp_timer_get_time();
                if ((!s_ready && now - connected_at > kReadyTimeoutUs) || now - last_rx > 45000000) {
                    ESP_LOGW(TAG, "voice session timed out");
                    retryable = true;
                    break;
                }
                if (now - last_ping > 15000000 && !call.WriteBlocked()) {
                    errno = 0;
                    const int ping = esp_transport_ws_send_raw(ws, static_cast<ws_transport_opcodes_t>(
                            WS_TRANSPORT_OPCODES_PING | WS_TRANSPORT_OPCODES_FIN), nullptr, 0, kSendTimeoutMs);
                    if (ping < 0) {
                        LogTransportFailure(tls, "ping", ping, errno);
                        retryable = true;
                        break;
                    }
                    last_ping = now;
                }
                SetWorkerPhase(2);
                errno = 0;
                call.RecordWsPoll();
                const int polled = esp_transport_poll_read(ws, call.TxPending() && !call.WriteBlocked() ? 0 : 10);
                SetWorkerPhase(0);
                if (polled < 0) {
                    LogTransportFailure(tls, "poll read", polled, errno);
                    retryable = true;
                    break;
                }
                const uint32_t sent_frames = s_tx_sent_frames.load();
                if (recovery.Observe(s_ready.load(), sent_frames != observed_tx, false,
                                     esp_timer_get_time() / 1000))
                    ESP_LOGI(TAG, "voice retry budget reset: READY and bidirectional progress stable for 120s");
                observed_tx = sent_frames;
                if (!polled) continue;
                SetWorkerPhase(3);
                errno = 0;
                const int64_t read_started_us = esp_timer_get_time();
                const int got = esp_transport_read(ws, reinterpret_cast<char*>(chunk), sizeof scratch->read, 1000);
                const int read_errno = errno;
                const int64_t read_finished_us = esp_timer_get_time();
                SetWorkerPhase(0);
                call.RecordWsRead(got, read_started_us, read_finished_us);
                if (got < 0) {
                    LogTransportFailure(tls, "read", got, read_errno);
                    retryable = true;
                    break;
                }
                if (!Current(grant.epoch)) break;
                s_rx_bytes.fetch_add(got);
                const auto opcode = esp_transport_ws_get_read_opcode(ws);
                const int frame_size = esp_transport_ws_get_read_payload_len(ws);
                const bool first = frame_read == 0;
                frame_read += got;
                const bool end = frame_read == frame_size;
                if (frame_size < 0 || frame_read > frame_size || (got == 0 && !end)) break;
                // An empty ping/pong is still a real received WebSocket frame.
                if (recovery.Observe(s_ready.load(), false, got > 0 || end, esp_timer_get_time() / 1000))
                    ESP_LOGI(TAG, "voice retry budget reset: READY and bidirectional progress stable for 120s");
                last_rx = esp_timer_get_time();
                if (opcode == WS_TRANSPORT_OPCODES_CLOSE) {
                    // Log only the numeric close code, never a server-supplied reason.
                    const unsigned code = got >= 2 ? (static_cast<unsigned>(chunk[0]) << 8) | chunk[1] : 0;
                    ESP_LOGW(TAG, "voice peer closed (code=%u); automatic retry disabled", code);
                    break;
                }
                if (opcode == WS_TRANSPORT_OPCODES_PING || opcode == WS_TRANSPORT_OPCODES_PONG) {
                    if (!first || !end || frame_size > 125) break;
                    if (opcode == WS_TRANSPORT_OPCODES_PING) {
                        errno = 0;
                        const int pong = esp_transport_ws_send_raw(ws,
                            static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_PONG | WS_TRANSPORT_OPCODES_FIN),
                            reinterpret_cast<char*>(chunk), got, kSendTimeoutMs);
                        if (pong < 0 || pong != got) {
                            LogTransportFailure(tls, "pong", pong, errno);
                            retryable = true;
                            break;
                        }
                    }
                } else {
                    const size_t buffered = assembler.Active() ? assembler.Size() : 0;
                    if (!message.Reserve(buffered + static_cast<size_t>(got))) {
                        ESP_LOGW(TAG, "voice message allocation/limit failed (%u bytes, largest=%u)",
                                 static_cast<unsigned>(buffered + got),
                                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
                        break;
                    }
                    assembler.Rebind(message.Data(), message.Capacity());
                    const auto result = assembler.Append(opcode, first, end,
                            esp_transport_ws_get_fin_flag(ws), chunk, static_cast<size_t>(got));
                    if (result == agentlink::realtime::Message::kError) {
                        ESP_LOGW(TAG, "invalid or oversized voice frame");
                        break;
                    }
                    if (result == agentlink::realtime::Message::kComplete) {
                        call.RecordWsMessage();
                        // Cancel queued speech before an interruption waits behind paced audio.
                        if (assembler.Opcode() == WS_TRANSPORT_OPCODES_TEXT)
                            CheckResponseInterruption(message.Data(), assembler.Size(), esp_timer_get_time(), grant.token);
                        pending = true;
                        pending_at = esp_timer_get_time();
                        pending_reset = s_reset.load();
                    }
                }
                if (end) frame_read = 0;
            }
            call.ReportWriteStall("connection_end", esp_timer_get_time());
            // An explicit server rejection or invalid media must not be retried
            // merely because the network worker observed a simultaneous failure.
            if (!call.Healthy()) retryable = false;
        }
        call.Stop();
        memset(hello, 0, sizeof hello);
        // A failed write may have left a partial frame. Do not append session.end.
        if (!retryable) Send(sender, "{\"type\":\"session.end\"}");
        esp_transport_close(ws);
    }
    free(scratch);
    esp_transport_destroy(ws);
    esp_transport_destroy(tls);
    return retryable;
}
#endif

void FinishGrant(uint32_t epoch) {
    // Cancellation/new requests also hold s_mutex. An old worker must not clear
    // the transport's running flag after a newer grant has been queued.
    std::lock_guard<std::mutex> lock(s_mutex);
    if (Current(epoch)) {
        s_recovering.store(false);
        if (s_cb.finished) s_cb.finished();
    }
}

bool SameRoute(const Grant& grant, int agent_id, const char* session_id) {
    return grant.agent_id == agent_id && strcmp(grant.session_id, session_id) == 0;
}

// The worker serializes HTTPS renewal and consumption. A call queued during a
// refresh can use its result, and no background job ever enters ConnectOnce().
void MaintainPreparedGrant() {
    Grant next = {};
    uint32_t generation = 0;
    bool refresh = false;
    int64_t previous_expiry_us = 0;
    AudioFormat previous_audio{};
    const int64_t now = esp_timer_get_time();
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_prepared.expires_us <= now) s_prepared = {};
        if (now >= s_prepare.maintain_until_us || now < s_prepare.next_attempt_us ||
            uxQueueMessagesWaiting(s_grants)) return;
        next.platform = true;
        next.agent_id = s_prepare.agent_id;
        memcpy(next.session_id, s_prepare.session_id, sizeof next.session_id);
        generation = s_prepare.generation;
        refresh = SameRoute(s_prepared, next.agent_id, next.session_id);
        // A refresh may invalidate the previous token even if its response is lost.
        if (refresh) {
            previous_expiry_us = s_prepared.expires_us;
            previous_audio = s_prepared.audio;
            s_prepared = {};
        }
    }
    int status = 0;
    int business_code = 0;
    const auto fetch = [&](bool renew) {
        return cloud_platform_grant(next.agent_id, next.session_id, renew,
                                    next.url, sizeof next.url, next.token, sizeof next.token,
                                    &next.audio, &status, &next.expires_us, renew ? &previous_audio : nullptr,
                                    renew ? &business_code : nullptr);
    };
    esp_err_t result = fetch(refresh);
    const bool ended = refresh && result != ESP_OK && business_code == kVoiceGrantSessionEnded;
    const bool unsupported = refresh && result == ESP_ERR_NOT_SUPPORTED && next.url[0] && next.token[0];
    if (ended || unsupported) {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (generation != s_prepare.generation || !s_run.load()) return;
        }
        if (ended) {
            ESP_LOGI(TAG, "Voice grant cache: renewal code=%d; immediately requesting a fresh grant", business_code);
        } else {
            ESP_LOGW(TAG, "Voice grant audio: replacing unsupported renewal with a fresh grant");
            cloud_platform_end_grant(next.agent_id, next.session_id);
        }
        memset(next.url, 0, sizeof next.url);
        memset(next.token, 0, sizeof next.token);
        next.audio = {};
        result = fetch(false);
    }
    const int64_t finished = esp_timer_get_time();
    const int64_t remaining_us = std::max<int64_t>(0, next.expires_us - finished);
    const bool valid = result == ESP_OK && remaining_us > kPreparedUseMarginUs;
    ESP_LOGI(TAG, "Voice grant cache: action=%s ms=%u http=%d result=%s remaining_ms=%u previous_remaining_ms=%d usable=%s",
             ended || unsupported ? "regrant" : refresh ? "renew" : "prefetch",
             static_cast<unsigned>((finished - now) / 1000), status,
             esp_err_to_name(result), static_cast<unsigned>(remaining_us / 1000),
             refresh ? static_cast<int>((previous_expiry_us - finished) / 1000) : -1, valid ? "yes" : "no");
    // A successful reservation with no usable expiry must not block retries.
    if (!valid && result == ESP_OK) cloud_platform_end_grant(next.agent_id, next.session_id);
    std::lock_guard<std::mutex> lock(s_mutex);
    if (generation != s_prepare.generation || !s_run.load()) return;
    if (valid) {
        s_prepared = next;
#if CONFIG_IDF_TARGET_ESP32S3
        PrefetchVoiceDns(next.url);
#endif
        const int64_t lead = std::min<int64_t>(15000000, (remaining_us + kPreparedUseMarginUs) / 2);
        s_prepare.next_attempt_us = next.expires_us - lead;
    } else {
        s_prepare.next_attempt_us = esp_timer_get_time() + kPrepareRetryUs;
    }
}

bool TakePreparedGrant(Grant* grant) {
    std::lock_guard<std::mutex> lock(s_mutex);
    const int64_t remaining_us = s_prepared.expires_us - esp_timer_get_time();
    if (remaining_us <= kPreparedUseMarginUs ||
        !SameRoute(s_prepared, grant->agent_id, grant->session_id)) return false;
    const uint32_t epoch = grant->epoch;
    *grant = s_prepared;
    grant->epoch = epoch;
    s_prepared = {};
    ESP_LOGI(TAG, "Voice stage grant: source=cache remaining_ms=%u", static_cast<unsigned>(remaining_us / 1000));
    return true;
}

void Worker(void*) {
    Grant grant = {};
    while (s_run.load(std::memory_order_acquire)) {
        if (xQueueReceive(s_grants, &grant, pdMS_TO_TICKS(250)) != pdTRUE) {
            MaintainPreparedGrant();
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (!Current(grant.epoch)) continue;
            s_busy.store(true);
        }
        const char* hint = "Voice ended. Tap Connect to retry";
        bool cached = grant.platform && TakePreparedGrant(&grant);
        agentlink::realtime::RecoverTransport([&](agentlink::realtime::RetryBudget& recovery) {
            bool granted = cached || (!grant.direct && !grant.platform);
#if CONFIG_AGENT_LINK_REALTIME_DIRECT
            if (grant.direct && Current(grant.epoch)) granted = FetchDirectGrant(&grant, &hint);
#endif
            if (grant.platform && !cached && Current(grant.epoch)) {
                int status = 0;
                int64_t started = esp_timer_get_time();
                esp_err_t result = cloud_platform_grant(grant.agent_id, grant.session_id, false,
                                                        grant.url, sizeof grant.url, grant.token, sizeof grant.token,
                                                        &grant.audio, &status, &grant.expires_us);
                ESP_LOGI(TAG, "Voice stage grant: source=network ms=%u http=%d result=%s",
                         static_cast<unsigned>((esp_timer_get_time() - started) / 1000), status, esp_err_to_name(result));
                granted = result == ESP_OK;
                if (!granted && status == 409 && Current(grant.epoch)) {
                    started = esp_timer_get_time();
                    result = cloud_platform_grant(grant.agent_id, grant.session_id, true,
                                                  grant.url, sizeof grant.url, grant.token, sizeof grant.token,
                                                  &grant.audio, &status, &grant.expires_us);
                    ESP_LOGI(TAG, "Voice stage grant refresh: ms=%u http=%d result=%s",
                             static_cast<unsigned>((esp_timer_get_time() - started) / 1000), status, esp_err_to_name(result));
                    granted = result == ESP_OK;
                }
                if (!granted) hint = status == 409 ? "Voice session busy. End the previous call"
                                     : "Voice access failed. Tap Connect to retry";
            }
            bool retryable = false;
            if (granted && Current(grant.epoch)) retryable = ConnectOnce(grant, recovery);
            s_ready.store(false);
            ResetInput();
            if (s_cb.audio_reset) {
                const auto reason = !Current(grant.epoch) ? AGENT_AUDIO_RESET_CONNECTION_CANCELLED :
                                    retryable ? AGENT_AUDIO_RESET_CONNECTION_LOST : AGENT_AUDIO_RESET_CONNECTION_ENDED;
                s_cb.audio_reset(reason, 0);
            }
            // Notify applications before cleanup/backoff so the mic stops while offline.
            const bool retry = grant.platform && retryable && recovery.CanRetry() && Current(grant.epoch);
            SetRecovering(grant.epoch, retry);
            if (retryable) hint = "Voice network failed. Tap Connect to retry";
            if (s_cb.state) s_cb.state(false, retry ? "Voice interrupted. Reconnecting..." : hint);
            if (grant.platform && (granted || (grant.url[0] && grant.token[0])))
                cloud_platform_end_grant(grant.agent_id, grant.session_id,
                                         retryable ? CloudGrantCleanup::kBeforeRetry : CloudGrantCleanup::kNormal);
#if CONFIG_AGENT_LINK_REALTIME_DIRECT
            if (grant.direct && granted) EndDirectSession();
#endif
            return retry && Current(grant.epoch);
        }, [&](unsigned retries, unsigned delay_ms) {
            ESP_LOGW(TAG, "voice transport recovery %u/2 in %u ms (consecutive failures; fresh grant, old audio discarded)",
                     retries, delay_ms);
            const int64_t until = esp_timer_get_time() + delay_ms * 1000LL;
            while (Current(grant.epoch) && esp_timer_get_time() < until) vTaskDelay(pdMS_TO_TICKS(50));
            if (!Current(grant.epoch)) return false;
            // Tokens are one-use. Obtain a fresh grant on the next iteration.
            cached = false;
            grant.expires_us = 0;
            memset(grant.url, 0, sizeof grant.url);
            memset(grant.token, 0, sizeof grant.token);
            return true;
        });
        FinishGrant(grant.epoch);
        s_busy.store(false);
        grant = {};
    }
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    s_wss_ticket.session.reset();
#endif
    s_task.store(nullptr, std::memory_order_release);
#if !AGENT_LINK_STATIC_VOICE_PSRAM
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM
    vTaskDeleteWithCaps(nullptr);
#else
    vTaskDelete(nullptr);
#endif
#endif
}

#if AGENT_LINK_STATIC_VOICE_PSRAM
void ResidentWorker(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        Worker(nullptr);
    }
}
#endif
}  // namespace

namespace {
bool StartWorker() {
    if (s_task.load(std::memory_order_acquire)) return true;
    TaskHandle_t task = nullptr;
#if CONFIG_IDF_TARGET_ESP32C3
    constexpr unsigned stack_bytes = 40960;
#else
    constexpr unsigned stack_bytes = 12288;
#endif
#if AGENT_LINK_STATIC_VOICE_PSRAM
    EXT_RAM_BSS_ATTR alignas(16) static StackType_t realtime_stack[stack_bytes];
    static StaticTask_t realtime_tcb;
    static const TaskHandle_t worker = xTaskCreateStatic(&ResidentWorker, "agentlink_rt", sizeof(realtime_stack),
                                                          nullptr, 5, realtime_stack, &realtime_tcb);
    if (!worker) return false;
    task = worker;
#elif CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM
    if (xTaskCreateWithCaps(&Worker, "agentlink_rt", stack_bytes, nullptr, 5, &task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) return false;
#else
    if (xTaskCreate(&Worker, "agentlink_rt", stack_bytes, nullptr, 5, &task) != pdPASS) return false;
#endif
    s_task.store(task, std::memory_order_release);
#if AGENT_LINK_STATIC_VOICE_PSRAM
    xTaskNotifyGive(task);
#endif
#if CONFIG_IDF_TARGET_ESP32C3
    ESP_LOGD(TAG, "resident voice task ready: stack=%uB, internal free=%u largest=%u",
             stack_bytes,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
#endif
    return true;
}

// Called with s_mutex held; both grant sources share the same worker and queue.
esp_err_t QueueGrant(Grant* grant) {
    grant->epoch = s_epoch.fetch_add(1) + 1;
    s_ready.store(false);
    s_continuous.store(grant->platform);
    s_recovering.store(false);
    if (xQueueOverwrite(s_grants, grant) != pdPASS) return ESP_FAIL;
    if (!StartWorker()) {
        xQueueReset(s_grants);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
}  // namespace

esp_err_t cloud_realtime_start(void) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_grants) return ESP_OK;
#if AGENT_LINK_STATIC_VOICE_PSRAM
    s_grants = xQueueCreateStatic(1, sizeof(Grant), s_grant_storage, &s_grant_queue);
    s_tx = xQueueCreateStatic(kTxDepth, sizeof(Tx), reinterpret_cast<uint8_t*>(s_tx_storage), &s_tx_queue);
    if (!s_grants || !s_tx) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Static PSRAM WSS: message=%uB receive=%uB uplink=%uB scratch=%uB",
             static_cast<unsigned>(sizeof(s_ws_message)), static_cast<unsigned>(sizeof(s_rx_storage)),
             static_cast<unsigned>(sizeof(s_tx_storage)), static_cast<unsigned>(sizeof(s_ws_scratch)));
#else
    s_grants = xQueueCreateWithCaps(1, sizeof(Grant), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_grants_caps = s_grants != nullptr;
    if (!s_grants) s_grants = xQueueCreate(1, sizeof(Grant));
    if (!s_grants) return ESP_ERR_NO_MEM;
    s_tx = xQueueCreateWithCaps(kTxDepth, sizeof(Tx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tx_caps = s_tx != nullptr;
    if (!s_tx) s_tx = xQueueCreate(kTxDepth, sizeof(Tx));
    if (!s_tx) {
        if (s_grants_caps) vQueueDeleteWithCaps(s_grants);
        else vQueueDelete(s_grants);
        s_grants = nullptr;
        s_grants_caps = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGD(TAG, "voice queues: tx=%s grant=%s, internal free=%u largest=%u",
             s_tx_caps ? "PSRAM" : "internal", s_grants_caps ? "PSRAM" : "internal",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
#endif
    s_run.store(true, std::memory_order_release);
#if CONFIG_IDF_TARGET_ESP32C3
    // Reserve the only large stack at SDK startup, before HTTPS fragments RAM.
    // It waits on the grant queue between calls and is deleted only at SDK stop.
    if (!StartWorker()) {
        s_run.store(false);
        if (s_tx_caps) vQueueDeleteWithCaps(s_tx); else vQueueDelete(s_tx);
        if (s_grants_caps) vQueueDeleteWithCaps(s_grants); else vQueueDelete(s_grants);
        s_tx = s_grants = nullptr;
        s_tx_caps = s_grants_caps = false;
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}

void cloud_realtime_stop(void) {
    if (!s_grants) return;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_run.store(false, std::memory_order_release);
        s_recovering.store(false);
        s_prepare = {};
        s_prepared = {};
        s_epoch.fetch_add(1);
        s_ready.store(false);
    }
    while (s_task.load(std::memory_order_acquire)) vTaskDelay(pdMS_TO_TICKS(50));
    cloud_realtime_invalidate_dns();
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_grants_caps) vQueueDeleteWithCaps(s_grants);
    else vQueueDelete(s_grants);
    s_grants = nullptr;
    s_grants_caps = false;
    // WithCaps buffers must be released with the matching API.
    if (s_tx_caps) vQueueDeleteWithCaps(s_tx);
    else vQueueDelete(s_tx);
    s_tx = nullptr;
    s_input = AGENT_STREAM_KIND_COUNT;
    s_continuous.store(false);
}

esp_err_t cloud_realtime_offer(const char* websocket_url, const char* connect_token) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_grants || !s_run) return ESP_ERR_INVALID_STATE;
    if (!websocket_url || strlen(websocket_url) >= sizeof Grant::url || !SafeToken(connect_token)) {
        return ESP_ERR_INVALID_ARG;
    }
    WssAddress parsed = {};
    if (!ParseWss(websocket_url, &parsed)) return ESP_ERR_INVALID_ARG;
    Grant grant = {};
    snprintf(grant.url, sizeof grant.url, "%s", websocket_url);
    snprintf(grant.token, sizeof grant.token, "%s", connect_token);
    const esp_err_t result = QueueGrant(&grant);
    grant = {};
    return result;
}

bool cloud_realtime_direct_enabled(void) {
#if CONFIG_AGENT_LINK_REALTIME_DIRECT
    return true;
#else
    return false;
#endif
}

esp_err_t cloud_realtime_request_direct(void) {
#if CONFIG_AGENT_LINK_REALTIME_DIRECT
    if (!DirectConfigValid()) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_grants || !s_run) return ESP_ERR_INVALID_STATE;
    Grant grant = {};
    grant.direct = true;
    return QueueGrant(&grant);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t cloud_realtime_request_platform(int agent_id, const char* session_id, bool standby) {
    if (!cloud_platform_enabled() || agent_id <= 0 || !session_id || !*session_id ||
        strlen(session_id) >= sizeof Grant::session_id) return ESP_ERR_INVALID_ARG;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(session_id); *p; ++p) {
        if (!std::isalnum(*p) && *p != '_' && *p != '-') return ESP_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_grants || !s_run) return ESP_ERR_INVALID_STATE;
    if (standby) {
        if (!StartWorker()) return ESP_ERR_NO_MEM;
        if (s_prepare.agent_id != agent_id || strcmp(s_prepare.session_id, session_id) != 0) {
            s_prepare = {agent_id, {}, 0, 0, s_prepare.generation + 1};
            memcpy(s_prepare.session_id, session_id, strlen(session_id) + 1);
        }
        s_prepare.maintain_until_us = esp_timer_get_time() + kPrepareLeaseUs;
        return ESP_OK;
    }
    Grant grant = {};
    grant.direct = false;
    grant.platform = true;
    grant.agent_id = agent_id;
    snprintf(grant.session_id, sizeof grant.session_id, "%s", session_id);
    return QueueGrant(&grant);
}

void cloud_realtime_set_callbacks(const cloud_realtime_callbacks_t* callbacks) {
    s_cb = callbacks ? *callbacks : cloud_realtime_callbacks_t{};
}

extern "C" void agent_link_platform_set_user_transcript_callback(agent_link_platform_user_transcript_cb_t callback) {
    s_user_transcript = callback;
}

extern "C" void agent_link_platform_set_voice_timing_callback(agent_link_platform_voice_timing_cb_t callback) {
    s_voice_timing = callback;
}

extern "C" void agent_link_platform_set_voice_trace([[maybe_unused]] agent_link_platform_voice_detector_t detector,
                                                     [[maybe_unused]] agent_link_platform_voice_trace_cb_t callback) {
#if CONFIG_IDF_TARGET_ESP32S3
    s_voice_detector = detector;
    s_voice_trace = callback;
#endif
}

bool cloud_realtime_ready(void) { return s_ready.load(); }
bool cloud_realtime_recovering(void) { return s_recovering.load(); }

void cloud_realtime_invalidate_dns(void) {
#if CONFIG_IDF_TARGET_ESP32S3
    std::lock_guard<std::mutex> lock(s_voice_dns_mutex);
    s_voice_dns.expires_us = 0;
    ++s_voice_dns.generation;
#endif
}

void cloud_realtime_cancel(void) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_epoch.fetch_add(1);
    s_ready.store(false);
    s_mute.store(true);
    s_recovering.store(false);
    ResetInputLocked();
    s_prepare = {0, {}, 0, 0, s_prepare.generation + 1};
    s_prepared = {};
    if (s_grants) xQueueReset(s_grants);
}

esp_err_t cloud_realtime_wait_idle(uint32_t timeout_ms) {
    const int64_t until = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    while (s_busy.load()) {
        if (esp_timer_get_time() >= until) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

esp_err_t cloud_realtime_stream_start(agent_stream_t type) {
    if (type != AGENT_STREAM_VOICE && type != AGENT_STREAM_AUDIO) return ESP_ERR_NOT_SUPPORTED;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_run || !s_ready || s_input != AGENT_STREAM_KIND_COUNT) return ESP_ERR_INVALID_STATE;
    if (s_continuous) {
        s_tx_sent_frames.store(0);
        s_tx_sent_messages.store(0);
        s_tx_send_max_ms.store(0);
        s_tx_would_block.store(0); s_tx_partial.store(0); s_tx_select0_accepted.store(0);
        s_tx_bytes.store(0);
        s_rx_bytes.store(0);
        s_decoded_samples.store(0);
        s_tx_send_ms.store(0);
        s_tx_write_calls.store(0);
        s_tx_encode_max_us.store(0);
        s_encoder_stack.store(0);
        s_diag_last_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        s_diag_mic_frames = s_diag_dropped_frames = 0;
        s_diag_prev_sent_frames = s_diag_prev_sent_messages = 0;
        s_diag_prev_tx_bytes = s_diag_prev_rx_bytes = s_diag_prev_samples = 0;
        s_input = type;
#if CONFIG_IDF_TARGET_ESP32S3
        s_capture_turn.fetch_add(1);
        s_mute.store(false);
        ESP_LOGI(TAG, "Resident voice microphone: on");
#endif
        return ESP_OK;
    }
    if (uxQueueSpacesAvailable(s_tx) < 3) return ESP_ERR_NO_MEM;
    const uint32_t turn = AdvanceAudioReset(AGENT_AUDIO_RESET_INPUT_START);
    const Tx start = {TxKind::kStart, 0, turn, {}};
    if (xQueueSend(s_tx, &start, 0) != pdTRUE) return ESP_ERR_NO_MEM;
    s_input = type;
    s_mute.store(true);
    return ESP_OK;
}

esp_err_t cloud_realtime_stream_write(agent_stream_t type, const uint8_t* data, size_t bytes) {
    if (!data || !bytes || bytes % 2) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_run || !s_ready || s_input != type) return ESP_ERR_INVALID_STATE;
    const size_t packets = (bytes + kMicBytes - 1) / kMicBytes;
    if (s_continuous) s_diag_mic_frames += static_cast<uint32_t>(packets);
    if (packets + 2 > uxQueueSpacesAvailable(s_tx)) {
        if (s_continuous) {
            s_diag_dropped_frames += static_cast<uint32_t>(packets);
            ReportUplink();
        }
        return ESP_ERR_NO_MEM;
    }
    Tx packet = {TxKind::kPcm, 0, s_capture_turn.load(), {}};
    while (bytes) {
        packet.bytes = bytes > kMicBytes ? kMicBytes : bytes;
        memcpy(packet.data, data, packet.bytes);
        if (xQueueSend(s_tx, &packet, 0) != pdTRUE) {
            if (s_continuous) {
                ++s_diag_dropped_frames;
                ReportUplink();
            }
            return ESP_ERR_NO_MEM;
        }
        data += packet.bytes;
        bytes -= packet.bytes;
    }
    if (s_continuous) ReportUplink();
    return ESP_OK;
}

esp_err_t cloud_realtime_stream_end(agent_stream_t type, bool complete) {
    if (!complete) { cloud_realtime_cancel(); return ESP_OK; }
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_run || s_input != type) return ESP_ERR_INVALID_STATE;
#if CONFIG_IDF_TARGET_ESP32S3
    if (s_continuous) {
        s_mute.store(true);
        ResetInputLocked();
        AdvanceAudioReset(AGENT_AUDIO_RESET_INPUT_CLOSED);
        ESP_LOGI(TAG, "Resident voice microphone: off; audio_frames=%u",
                 static_cast<unsigned>(s_tx_sent_frames.load()));
        return ESP_OK;
    }
#endif
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (s_continuous) {
        s_input = AGENT_STREAM_KIND_COUNT;
        return ESP_OK;
    }
    const Tx end = {TxKind::kEnd, 0, s_reset.load(), {}};
    if (xQueueSend(s_tx, &end, 0) != pdTRUE) return ESP_ERR_NO_MEM;
    s_input = AGENT_STREAM_KIND_COUNT;
    return ESP_OK;
}
