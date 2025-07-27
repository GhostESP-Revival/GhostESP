#include "core/cli-handlers/sd-cli-handler.h"
#include "managers/sd_card_manager.h"
#include "managers/views/terminal_screen.h"
#include <stdio.h>
#include <stdlib.h>

void handle_sd_config(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: sd_config\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: sd_config\n");
        return;
    }
    sd_card_print_config();
}

void handle_sd_pins_mmc(int argc, char **argv) {
    if (argc != 7) {
        printf("Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n");
        printf("Sets pins for SDMMC mode (only effective if compiled for MMC).\n");
        TERMINAL_VIEW_ADD_TEXT("Sets pins for SDMMC mode (only effective if compiled for MMC).\n");
        printf("Example: sd_pins_mmc 19 18 20 21 22 23\n");
        TERMINAL_VIEW_ADD_TEXT("Example: sd_pins_mmc 19 18 20 21 22 23\n");
        return;
    }

    int clk = atoi(argv[1]);
    int cmd = atoi(argv[2]);
    int d0 = atoi(argv[3]);
    int d1 = atoi(argv[4]);
    int d2 = atoi(argv[5]);
    int d3 = atoi(argv[6]);

    if (clk < 0 || cmd < 0 || d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0 ||
        clk > 40 || cmd > 40 || d0 > 40 || d1 > 40 || d2 > 40 || d3 > 40) {
        printf("Invalid GPIO pins. Pins must be between 0 and 40.\n");
        TERMINAL_VIEW_ADD_TEXT("Invalid GPIO pins. Pins must be between 0 and 40.\n");
        return;
    }

    sd_card_set_mmc_pins(clk, cmd, d0, d1, d2, d3);
}

void handle_sd_pins_spi(int argc, char **argv) {
    if (argc != 5) {
        printf("Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n");
        printf("Sets pins for SPI mode (only effective if compiled for SPI).\n");
        TERMINAL_VIEW_ADD_TEXT("Sets pins for SPI mode (only effective if compiled for SPI).\n");
        printf("Example: sd_pins_spi 5 18 19 23\n");
        TERMINAL_VIEW_ADD_TEXT("Example: sd_pins_spi 5 18 19 23\n");
        return;
    }

    int cs = atoi(argv[1]);
    int clk = atoi(argv[2]);
    int miso = atoi(argv[3]);
    int mosi = atoi(argv[4]);

    if (cs < 0 || clk < 0 || miso < 0 || mosi < 0 ||
        cs > 40 || clk > 40 || miso > 40 || mosi > 40) {
        printf("Invalid GPIO pins. Pins must be between 0 and 40.\n");
        TERMINAL_VIEW_ADD_TEXT("Invalid GPIO pins. Pins must be between 0 and 40.\n");
        return;
    }

    sd_card_set_spi_pins(cs, clk, miso, mosi);
}

void handle_sd_save_config(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: sd_save_config\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: sd_save_config\n");
        return;
    }
    sd_card_save_config();
}