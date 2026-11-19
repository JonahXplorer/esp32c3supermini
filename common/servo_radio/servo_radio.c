#include "servo_radio.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "servo_radio";
static const uint32_t PROTOCOL_ID = 0x31565253;
static uint8_t peer_address[ESP_NOW_ETH_ALEN];
static QueueHandle_t inbox;

_Static_assert(sizeof(servo_message_t) == 16, "Unexpected ESP-NOW packet layout");

static void receive_packet(const esp_now_recv_info_t *info, const uint8_t *data, int length)
{
    if (length != sizeof(servo_message_t) ||
        memcmp(info->src_addr, peer_address, sizeof(peer_address)) != 0) {
        return;
    }
    servo_message_t message;
    memcpy(&message, data, sizeof(message));
    if (message.protocol == PROTOCOL_ID) {
        xQueueSend(inbox, &message, 0);
    }
}

esp_err_t servo_radio_start(const char *peer_mac)
{
    ESP_RETURN_ON_FALSE(strlen(peer_mac) == 17 &&
                        sscanf(peer_mac, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
                               &peer_address[0], &peer_address[1], &peer_address[2],
                               &peer_address[3], &peer_address[4], &peer_address[5]) == 6,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid peer MAC address");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Network initialization");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Event loop");
    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    wifi.nvs_enable = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi), TAG, "Wi-Fi initialization");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "Wi-Fi power save");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(SERVO_RADIO_CHANNEL, WIFI_SECOND_CHAN_NONE),
                        TAG, "ESP-NOW channel");
    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "ESP-NOW initialization");

    esp_now_peer_info_t peer = {.channel = SERVO_RADIO_CHANNEL, .ifidx = WIFI_IF_STA};
    memcpy(peer.peer_addr, peer_address, sizeof(peer_address));
    ESP_RETURN_ON_ERROR(esp_now_add_peer(&peer), TAG, "Add ESP-NOW peer");
    inbox = xQueueCreate(8, sizeof(servo_message_t));
    ESP_RETURN_ON_FALSE(inbox != NULL, ESP_ERR_NO_MEM, TAG, "ESP-NOW inbox");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(receive_packet), TAG, "ESP-NOW receive callback");

    uint8_t local[ESP_NOW_ETH_ALEN];
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, local), TAG, "Read local MAC");
    ESP_LOGI(TAG, "ESP-NOW channel %d, local %02X:%02X:%02X:%02X:%02X:%02X, peer %s",
             SERVO_RADIO_CHANNEL, local[0], local[1], local[2], local[3], local[4], local[5], peer_mac);
    return ESP_OK;
}

bool servo_radio_receive(servo_message_t *message)
{
    return xQueueReceive(inbox, message, 0) == pdTRUE;
}

esp_err_t servo_radio_send(servo_message_t message)
{
    message.protocol = PROTOCOL_ID;
    return esp_now_send(peer_address, (const uint8_t *)&message, sizeof(message));
}
