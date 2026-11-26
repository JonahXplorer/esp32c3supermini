#include "servo_radio.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

enum { SEARCH_DWELL_MS = 420, FOLLOW_TIMEOUT_MS = 1500, DIAGNOSTIC_DEPTH = 8 };

typedef struct {
    uint8_t first_channel;
    uint8_t last_channel;
    uint8_t current_channel;
    uint8_t candidate;
    bool locked;
    TickType_t last_activity;
} channel_follower_t;

typedef struct {
    servo_message_t message;
    uint8_t peer_index;
} received_packet_t;

typedef struct {
    servo_message_t message;
    uint8_t mac[ESP_NOW_ETH_ALEN];
    const char *stage;
    const char *result;
    int length;
    bool has_message;
    bool warning;
} radio_diagnostic_t;

static const char *TAG = "servo_radio";
static const uint32_t PROTOCOL_ID = 0x33565253;
static uint8_t peer_addresses[SERVO_RADIO_MAX_PEERS][ESP_NOW_ETH_ALEN];
static uint8_t peer_count;
static QueueHandle_t inbox;
static channel_follower_t follower;
static QueueHandle_t diagnostics;
static StaticQueue_t diagnostic_queue;
static uint8_t diagnostic_storage[DIAGNOSTIC_DEPTH * sizeof(radio_diagnostic_t)];
static atomic_uint dropped_diagnostics;

_Static_assert(sizeof(servo_message_t) == 20, "Unexpected ESP-NOW packet layout");

static void queue_diagnostic(const char *stage, const char *result, bool warning,
                             const uint8_t *mac, const servo_message_t *message, int length)
{
    if (!diagnostics) return;
    radio_diagnostic_t entry = {.stage = stage, .result = result, .length = length,
                                .has_message = message != NULL, .warning = warning};
    if (mac) memcpy(entry.mac, mac, sizeof(entry.mac));
    if (message) entry.message = *message;
    if (xQueueSend(diagnostics, &entry, 0) != pdTRUE) {
        atomic_fetch_add_explicit(&dropped_diagnostics, 1, memory_order_relaxed);
    }
}

void servo_radio_log_diagnostics(void)
{
    if (!diagnostics) return;
    radio_diagnostic_t entry;
    for (unsigned i = 0; i < DIAGNOSTIC_DEPTH && xQueueReceive(diagnostics, &entry, 0) == pdTRUE; ++i) {
        const esp_log_level_t level = entry.warning ? ESP_LOG_WARN : ESP_LOG_INFO;
        if (entry.has_message) {
            ESP_LOG_LEVEL(level, TAG, "ESP-NOW %s: result=%s session=%08" PRIx32 " seq=%" PRIu32
                          " mac=" MACSTR " ch=%u type=%u len=%d", entry.stage, entry.result,
                          entry.message.session, entry.message.sequence, MAC2STR(entry.mac),
                          (unsigned)entry.message.channel, (unsigned)entry.message.type, entry.length);
        } else {
            ESP_LOG_LEVEL(level, TAG, "ESP-NOW %s: result=%s session=? seq=? mac=" MACSTR " len=%d",
                          entry.stage, entry.result, MAC2STR(entry.mac), entry.length);
        }
    }
    const unsigned dropped = atomic_exchange_explicit(&dropped_diagnostics, 0, memory_order_relaxed);
    if (dropped) ESP_LOGW(TAG, "ESP-NOW diagnostic queue overflow: dropped=%u", dropped);
}

static void receive_packet(const esp_now_recv_info_t *info, const uint8_t *data, int length)
{
    uint8_t peer_index = 0;
    while (peer_index < peer_count &&
           memcmp(info->src_addr, peer_addresses[peer_index], ESP_NOW_ETH_ALEN) != 0) {
        ++peer_index;
    }
    const bool paired = peer_index < peer_count;
    if (length != sizeof(servo_message_t)) {
        if (paired) queue_diagnostic("RX rejected", "length", true, info->src_addr, NULL, length);
        return;
    }

    received_packet_t packet = {.peer_index = peer_index};
    memcpy(&packet.message, data, sizeof(packet.message));
    const char *rejection = NULL;
    if (!paired) {
        // Ignore unrelated ESP-NOW traffic from devices outside this pairing.
        if (packet.message.protocol != PROTOCOL_ID) return;
        rejection = "unpaired_mac";
    } else if (packet.message.protocol != PROTOCOL_ID) {
        rejection = "protocol";
    } else if (packet.message.channel < 1 || packet.message.channel > 14) {
        rejection = "invalid_channel";
    } else if (xQueueSend(inbox, &packet, 0) != pdTRUE) {
        rejection = "inbox_full";
    }
    if (rejection) {
        queue_diagnostic("RX rejected", rejection, true, info->src_addr, &packet.message, length);
    } else if (follower.first_channel == 0 && packet.message.type == SERVO_STATE) {
        queue_diagnostic("reply RX", "queued", false, info->src_addr, &packet.message, length);
    }
}

static void send_complete(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    enum { MAC_HEADER_BYTES = 24, ACTION_HEADER_BYTES = 8, ELEMENT_HEADER_BYTES = 7 };
    static const uint8_t action_prefix[] = {127, 0x18, 0xfe, 0x34};
    static const uint8_t element_prefix[] = {221, 5 + sizeof(servo_message_t), 0x18, 0xfe, 0x34, 4};
    servo_message_t message;
    const servo_message_t *identified = NULL;
    // Both ESP-NOW versions carry our 20-byte payload in one vendor element.
    // Match the callback's frame bytes so delayed or reordered callbacks still use the correct sequence.
    if (info && info->data && info->data_len >= ACTION_HEADER_BYTES + ELEMENT_HEADER_BYTES + sizeof(message)) {
        const uint8_t *action = info->data + MAC_HEADER_BYTES;
        const uint8_t *element = action + ACTION_HEADER_BYTES;
        if (memcmp(action, action_prefix, sizeof(action_prefix)) == 0 &&
            memcmp(element, element_prefix, sizeof(element_prefix)) == 0) {
            memcpy(&message, element + ELEMENT_HEADER_BYTES, sizeof(message));
            if (message.protocol == PROTOCOL_ID) identified = &message;
        }
    }
    const bool sent = status == ESP_NOW_SEND_SUCCESS;
    queue_diagnostic("TX callback", sent ? "mac_ack" : "no_mac_ack", !sent,
                     info ? info->des_addr : NULL, identified, info ? info->data_len : 0);
}

static bool read_peer_mac(const char *peer_mac, uint8_t address[ESP_NOW_ETH_ALEN])
{
    return peer_mac && strlen(peer_mac) == 17 &&
           sscanf(peer_mac, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
                  &address[0], &address[1], &address[2], &address[3], &address[4], &address[5]) == 6;
}

static esp_err_t configure_peers(const char *const peer_macs[], size_t count)
{
    ESP_RETURN_ON_FALSE(peer_macs && count > 0 && count <= SERVO_RADIO_MAX_PEERS,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid ESP-NOW peer count");
    ESP_RETURN_ON_FALSE(inbox == NULL, ESP_ERR_INVALID_STATE, TAG, "ESP-NOW already started");
    uint8_t addresses[SERVO_RADIO_MAX_PEERS][ESP_NOW_ETH_ALEN];
    for (size_t i = 0; i < count; ++i) {
        ESP_RETURN_ON_FALSE(read_peer_mac(peer_macs[i], addresses[i]),
                            ESP_ERR_INVALID_ARG, TAG, "Invalid ESP-NOW peer MAC at index %u", (unsigned)i);
    }
    if (count == 2 && memcmp(addresses[0], addresses[1], ESP_NOW_ETH_ALEN) == 0) count = 1;
    memcpy(peer_addresses, addresses, count * sizeof(addresses[0]));
    peer_count = (uint8_t)count;
    return ESP_OK;
}

static esp_err_t start_esp_now(void)
{
    uint8_t local[ESP_NOW_ETH_ALEN];
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, local), TAG, "Read local MAC");
    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "ESP-NOW initialization");

    // A zero peer channel sends on the interface's current channel.
    esp_err_t ret;
    esp_now_peer_info_t peer = {.channel = 0, .ifidx = WIFI_IF_STA};
    for (uint8_t i = 0; i < peer_count; ++i) {
        memcpy(peer.peer_addr, peer_addresses[i], sizeof(peer.peer_addr));
        ESP_GOTO_ON_ERROR(esp_now_add_peer(&peer), cleanup, TAG, "Add ESP-NOW peer %u", (unsigned)i);
    }
    inbox = xQueueCreate(8, sizeof(received_packet_t));
    ESP_GOTO_ON_FALSE(inbox != NULL, ESP_ERR_NO_MEM, cleanup, TAG, "ESP-NOW inbox");
    diagnostics = xQueueCreateStatic(DIAGNOSTIC_DEPTH, sizeof(radio_diagnostic_t),
                                     diagnostic_storage, &diagnostic_queue);
    ESP_GOTO_ON_ERROR(esp_now_register_recv_cb(receive_packet), cleanup, TAG, "ESP-NOW receive callback");
    const esp_err_t callback_result = esp_now_register_send_cb(send_complete);
    if (callback_result != ESP_OK) {
        ESP_LOGW(TAG, "ESP-NOW TX diagnostics unavailable: %s", esp_err_to_name(callback_result));
    }

    for (uint8_t i = 0; i < peer_count; ++i) {
        const uint8_t *address = peer_addresses[i];
        ESP_LOGI(TAG, "ESP-NOW local %02X:%02X:%02X:%02X:%02X:%02X, peer %02X:%02X:%02X:%02X:%02X:%02X",
                 local[0], local[1], local[2], local[3], local[4], local[5],
                 address[0], address[1], address[2], address[3], address[4], address[5]);
    }
    return ESP_OK;

cleanup:
    (void)esp_now_deinit();
    if (diagnostics) {
        vQueueDelete(diagnostics);
        diagnostics = NULL;
    }
    if (inbox) {
        vQueueDelete(inbox);
        inbox = NULL;
    }
    return ret;
}

esp_err_t servo_radio_attach(const char *peer_mac)
{
    // The Sugarat Wi-Fi owner has already started STA/APSTA and selects the home channel.
    ESP_RETURN_ON_ERROR(configure_peers(&peer_mac, 1), TAG, "ESP-NOW peer configuration");
    return start_esp_now();
}

static uint8_t next_channel(uint8_t channel)
{
    return channel < follower.last_channel ? channel + 1 : follower.first_channel;
}

esp_err_t servo_radio_start(const char *peer_mac, servo_radio_mode_t mode)
{
    return servo_radio_start_peers(&peer_mac, 1, mode);
}

esp_err_t servo_radio_start_peers(const char *const peer_macs[], size_t count, servo_radio_mode_t mode)
{
    ESP_RETURN_ON_ERROR(configure_peers(peer_macs, count), TAG, "ESP-NOW peer configuration");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Network initialization");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Event loop");
    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    wifi.nvs_enable = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi), TAG, "Wi-Fi initialization");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "Wi-Fi power save");
    uint8_t channel = SERVO_RADIO_START_CHANNEL;
    if (mode == SERVO_RADIO_FOLLOW_PEER) {
        wifi_country_t country;
        ESP_RETURN_ON_ERROR(esp_wifi_get_country(&country), TAG, "ESP-NOW channel range");
        follower.first_channel = country.schan;
        follower.last_channel = country.schan + country.nchan - 1;
        if (channel < follower.first_channel || channel > follower.last_channel) {
            channel = follower.first_channel;
        }
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE), TAG, "ESP-NOW channel");
    if (mode == SERVO_RADIO_FOLLOW_PEER) {
        follower.current_channel = channel;
        follower.candidate = next_channel(channel);
        follower.last_activity = xTaskGetTickCount();
        ESP_LOGI(TAG, "Looking for paired Sugarat on channels %u-%u, starting on %u",
                 follower.first_channel, follower.last_channel, channel);
    }
    return start_esp_now();
}

void servo_radio_scan_step(void)
{
    const TickType_t now = xTaskGetTickCount();
    const TickType_t dwell = pdMS_TO_TICKS(follower.locked ? FOLLOW_TIMEOUT_MS : SEARCH_DWELL_MS);
    if (follower.first_channel == 0 || now - follower.last_activity < dwell) {
        return;
    }
    if (follower.locked) {
        ESP_LOGW(TAG, "Sugarat timed out on channel %u; searching again", follower.current_channel);
        follower.locked = false;
    }
    const uint8_t channel = follower.candidate;
    follower.candidate = next_channel(channel);
    follower.last_activity = now;
    const esp_err_t result = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (result == ESP_OK) {
        follower.current_channel = channel;
    } else {
        ESP_LOGW(TAG, "Could not tune to channel %u: %s", channel, esp_err_to_name(result));
    }
}

static esp_err_t home_channel(uint8_t *channel)
{
    if (follower.current_channel != 0) {
        *channel = follower.current_channel;
        return ESP_OK;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        *channel = ap.primary;
        return ESP_OK;
    }
    wifi_second_chan_t secondary;
    return esp_wifi_get_channel(channel, &secondary);
}

bool servo_radio_receive(servo_message_t *message)
{
    return servo_radio_receive_from(message, NULL);
}

bool servo_radio_receive_from(servo_message_t *message, uint8_t *peer_index)
{
    received_packet_t packet;
    while (xQueueReceive(inbox, &packet, 0) == pdTRUE) {
        uint8_t channel;
        // Adjacent channels can overlap at short distances. Check the peer's advertised channel.
        const esp_err_t channel_result = home_channel(&channel);
        if (channel_result != ESP_OK) {
            ESP_LOGW(TAG, "ESP-NOW RX rejected: result=home_channel error=%s session=%08" PRIx32
                     " seq=%" PRIu32 " ch=%u", esp_err_to_name(channel_result), packet.message.session,
                     packet.message.sequence, (unsigned)packet.message.channel);
            continue;
        }
        if (packet.message.channel != channel) {
            ESP_LOGW(TAG, "ESP-NOW RX rejected: result=channel_mismatch session=%08" PRIx32
                     " seq=%" PRIu32 " ch=%u home_ch=%u", packet.message.session, packet.message.sequence,
                     (unsigned)packet.message.channel, (unsigned)channel);
            continue;
        }
        if (follower.first_channel != 0) {
            if (!servo_message_is_command(packet.message.type)) {
                ESP_LOGW(TAG, "ESP-NOW RX rejected: result=command_type session=%08" PRIx32
                         " seq=%" PRIu32 " type=%u", packet.message.session, packet.message.sequence,
                         (unsigned)packet.message.type);
                continue;
            }
            follower.last_activity = xTaskGetTickCount();
            if (!follower.locked) {
                follower.locked = true;
                ESP_LOGI(TAG, "Paired Sugarat found on channel %u", channel);
            }
        }
        *message = packet.message;
        if (peer_index) *peer_index = packet.peer_index;
        return true;
    }
    return false;
}

esp_err_t servo_radio_send(servo_message_t message)
{
    return servo_radio_send_to(0, message);
}

esp_err_t servo_radio_send_to(uint8_t peer_index, servo_message_t message)
{
    if (peer_index >= peer_count) return ESP_ERR_INVALID_ARG;
    message.protocol = PROTOCOL_ID;
    const esp_err_t err = home_channel(&message.channel);
    if (err != ESP_OK) {
        return err;
    }
    return esp_now_send(peer_addresses[peer_index], (const uint8_t *)&message, sizeof(message));
}
