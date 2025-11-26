#include "core/universal_ir.h"
#include <string.h>

bool universal_ir_get_signal(size_t index, infrared_signal_t *signal) {
    if (!signal || index >= UNIVERSAL_IR_SIGNAL_COUNT) {
        return false;
    }
    
    const universal_ir_signal_t *uni_signal = &universal_ir_signals[index];
    
    memset(signal, 0, sizeof(infrared_signal_t));
    strncpy(signal->name, uni_signal->name, sizeof(signal->name) - 1);
    signal->name[sizeof(signal->name) - 1] = '\0';
    
    signal->is_raw = false;
    strncpy(signal->payload.message.protocol, uni_signal->protocol, sizeof(signal->payload.message.protocol) - 1);
    signal->payload.message.protocol[sizeof(signal->payload.message.protocol) - 1] = '\0';
    signal->payload.message.address = uni_signal->address;
    signal->payload.message.command = uni_signal->command;
    
    return true;
}

size_t universal_ir_get_signal_count(void) {
    return UNIVERSAL_IR_SIGNAL_COUNT;
}

const char* universal_ir_get_signal_name(size_t index) {
    if (index >= UNIVERSAL_IR_SIGNAL_COUNT) {
        return NULL;
    }
    return universal_ir_signals[index].name;
}
