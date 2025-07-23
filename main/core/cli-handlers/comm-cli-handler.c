#include "core/cli-handlers/comm-cli-handler.h"
#include "core/esp_comm_manager.h"
#include "managers/settings_manager.h"
#include "managers/views/terminal_screen.h"
#include "driver/gpio.h"
#include <stdio.h>
#include <string.h>

extern FSettings G_Settings;

void handle_comm_discovery(int argc, char **argv) {
    comm_state_t state = esp_comm_manager_get_state();
    if (state == COMM_STATE_SCANNING) {
        printf("Already in discovery mode. Listening for peers...\n");
        TERMINAL_VIEW_ADD_TEXT("Already in discovery mode. Listening for peers...\n");
        return;
    }
    if (esp_comm_manager_start_discovery()) {
        printf("Started discovery mode. Listening for peers...\n");
        TERMINAL_VIEW_ADD_TEXT("Started discovery mode. Listening for peers...\n");
    } else {
        printf("Failed to start discovery. Check if already connected.\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to start discovery. Check if already connected.\n");
    }
}

void handle_comm_connect(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: commconnect <peer_name>\n");
        printf("Example: commconnect ESP_A1B2C3\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: commconnect <peer_name>\n");
        return;
    }
    if (esp_comm_manager_connect_to_peer(argv[1])) {
        printf("Attempting to connect to peer: %s\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Attempting to connect to peer...\n");
    } else {
        printf("Failed to connect. Make sure you're in discovery mode first.\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to connect. Start discovery first.\n");
    }
}

void handle_comm_send(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: commsend <command> [data]\n");
        printf("Example: commsend hello world\n");
        printf("Example: commsend scanap\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: commsend <command> [data]\n");
        return;
    }
    if (!esp_comm_manager_is_connected()) {
        printf("Not connected to any peer. Use 'commdiscovery' and 'commconnect' first.\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected. Connect to a peer first.\n");
        return;
    }
    char data_buffer[256] = {0};
    if (argc > 2) {
        int offset = 0;
        for (int i = 2; i < argc; i++) {
            int remaining = sizeof(data_buffer) - offset;
            int written = snprintf(data_buffer + offset, remaining, "%s ", argv[i]);
            if (written >= remaining) {
                printf("W: Command data truncated.\n");
                break;
            }
            offset += written;
        }
        if (offset > 0) {
            data_buffer[offset - 1] = '\0'; // Remove trailing space
        }
    }
    const char* command = argv[1];
    const char* data = (argc > 2) ? data_buffer : NULL;
    if (esp_comm_manager_send_command(command, data)) {
        if (data && data[0] != '\0') {
            printf("Command sent: %s %s\n", command, data);
        } else {
            printf("Command sent: %s\n", command);
        }
        TERMINAL_VIEW_ADD_TEXT("Command sent successfully.\n");
    } else {
        printf("Failed to send command.\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send command.\n");
    }
}

void handle_comm_status(int argc, char **argv) {
    comm_state_t state = esp_comm_manager_get_state();
    const char* state_str;
    switch(state) {
        case COMM_STATE_IDLE: state_str = "IDLE"; break;
        case COMM_STATE_SCANNING: state_str = "SCANNING"; break;
        case COMM_STATE_HANDSHAKE: state_str = "HANDSHAKE"; break;
        case COMM_STATE_CONNECTED: state_str = "CONNECTED"; break;
        case COMM_STATE_ERROR: state_str = "ERROR"; break;
        default: state_str = "UNKNOWN"; break;
    }
    printf("Communication Status: %s\n", state_str);
    if (esp_comm_manager_is_connected()) {
        printf("Connected to peer. Ready to send commands.\n");
        TERMINAL_VIEW_ADD_TEXT("Status: Connected\n");
    } else {
        printf("Not connected. Use 'commdiscovery' to find peers.\n");
        TERMINAL_VIEW_ADD_TEXT("Status: Not connected\n");
    }
}

void handle_comm_disconnect(int argc, char **argv) {
    esp_comm_manager_disconnect();
    printf("Disconnected from peer.\n");
    TERMINAL_VIEW_ADD_TEXT("Disconnected from peer.\n");
}

void handle_comm_setpins(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: commsetpins <tx_pin> <rx_pin>\n");
        printf("Example: commsetpins 4 5\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: commsetpins <tx_pin> <rx_pin>\n");
        return;
    }
    int tx_pin = atoi(argv[1]);
    int rx_pin = atoi(argv[2]);
    if (tx_pin < 0 || tx_pin > 48 || rx_pin < 0 || rx_pin > 48) {
        printf("Invalid pin numbers. Must be between 0-48.\n");
        TERMINAL_VIEW_ADD_TEXT("Invalid pin numbers.\n");
        return;
    }
    if (esp_comm_manager_set_pins((gpio_num_t)tx_pin, (gpio_num_t)rx_pin)) {
        settings_set_esp_comm_pins(&G_Settings, tx_pin, rx_pin);
        settings_save(&G_Settings);
        printf("Communication pins changed to TX:%d RX:%d and saved to NVS\n", tx_pin, rx_pin);
        TERMINAL_VIEW_ADD_TEXT("Communication pins changed and saved.\n");
    } else {
        printf("Failed to change pins. Make sure not connected or scanning.\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to change pins.\n");
    }
}