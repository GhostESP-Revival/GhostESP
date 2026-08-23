#pragma once

// 61137 - master flag to silence high-frequency noisy logs that spam serial.
// Set to 1 to suppress, 0 to re-enable for debugging.
// Controls: memory_debug allocation failures, DisplayManager input_queue backlog,
// and infrared allocation failures (which flood when browsing remotes with low heap).
#define SUPPRESS_NOISY_LOGS 1

#define SUPPRESS_MEMORY_DEBUG_LOGS SUPPRESS_NOISY_LOGS
#define SUPPRESS_DISPLAY_BACKLOG_LOGS SUPPRESS_NOISY_LOGS
#define SUPPRESS_IR_ALLOC_LOGS SUPPRESS_NOISY_LOGS
