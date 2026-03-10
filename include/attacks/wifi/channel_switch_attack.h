#ifndef CHANNEL_SWITCH_ATTACK_H
#define CHANNEL_SWITCH_ATTACK_H

#include <stdbool.h>
#include <stdint.h>

void channel_switch_attack_start(void);

void channel_switch_attack_stop(void);

bool channel_switch_attack_is_running(void);

uint32_t channel_switch_attack_get_packets_sent(void);

void channel_switch_attack_reset_packet_counter(void);

#endif
