// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
// Clean-room ST25R3916 register/command definitions transcribed from the public
// ST datasheet DS12484 Rev 8 (Table 13 direct commands, Table 17 Space-A
// registers, and the per-register bit tables). Register addresses and bit
// positions are hardware facts taken from that datasheet. No ST SLA0044 or
// third-party driver source is used.
//
// Target-mode bit values (PT_DEF, PT_MOD, MODE target bits) cross-checked
// against Flipper Zero Momentum-Firmware lib/drivers/st25r3916_reg.h (GPL-3.0,
// https://github.com/Next-Flip/Momentum-Firmware).

#ifndef ST25R3916_REG_H
#define ST25R3916_REG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Space-A registers (DS12484 Rev 8, Table 17) ===== */
#define ST25R3916_REG_IO_CONF1            0x00 /**< IO configuration register 1. */
#define ST25R3916_REG_IO_CONF2            0x01 /**< IO configuration register 2. */
#define ST25R3916_REG_OP_CONTROL          0x02 /**< Operation control register. */
#define ST25R3916_REG_MODE                0x03 /**< Mode definition register. */
#define ST25R3916_REG_BIT_RATE            0x04 /**< Bit rate definition register. */
#define ST25R3916_REG_ISO14443A_NFC       0x05 /**< ISO14443A and NFC 106 kb/s settings. */
#define ST25R3916_REG_ISO14443B_1         0x06 /**< ISO14443B settings register 1. */
#define ST25R3916_REG_ISO14443B_FELICA    0x07 /**< ISO14443B and FeliCa settings. */
#define ST25R3916_REG_PT_DEF              0x08 /**< NFCIP-1 passive target definition. */
#define ST25R3916_REG_STREAM_MODE         0x09 /**< Stream mode definition register. */
#define ST25R3916_REG_AUX                 0x0A /**< Auxiliary definition register. */
#define ST25R3916_REG_RX_CONF1            0x0B /**< Receiver configuration register 1. */
#define ST25R3916_REG_RX_CONF2            0x0C /**< Receiver configuration register 2. */
#define ST25R3916_REG_RX_CONF3            0x0D /**< Receiver configuration register 3. */
#define ST25R3916_REG_RX_CONF4            0x0E /**< Receiver configuration register 4. */
#define ST25R3916_REG_MASK_RX_TIMER       0x0F /**< Mask receive timer register. */
#define ST25R3916_REG_NO_RESPONSE_TIMER1  0x10 /**< No-response timer register 1. */
#define ST25R3916_REG_NO_RESPONSE_TIMER2  0x11 /**< No-response timer register 2. */
#define ST25R3916_REG_TIMER_EMV_CONTROL   0x12 /**< Timer and EMV control register. */
#define ST25R3916_REG_GPT1                0x13 /**< General purpose timer register 1. */
#define ST25R3916_REG_GPT2                0x14 /**< General purpose timer register 2. */
#define ST25R3916_REG_PPON2               0x15 /**< PPON2 field waiting register. */
#define ST25R3916_REG_MASK_MAIN_IRQ       0x16 /**< Mask main interrupt register. */
#define ST25R3916_REG_MASK_TIMER_NFC_IRQ  0x17 /**< Mask timer and NFC interrupt register. */
#define ST25R3916_REG_MASK_ERR_WUP_IRQ    0x18 /**< Mask error and wake-up interrupt register. */
#define ST25R3916_REG_MASK_PT_IRQ         0x19 /**< Mask passive target interrupt register. */
#define ST25R3916_REG_MAIN_IRQ            0x1A /**< Main interrupt register. */
#define ST25R3916_REG_TIMER_NFC_IRQ       0x1B /**< Timer and NFC interrupt register. */
#define ST25R3916_REG_ERROR_IRQ           0x1C /**< Error and wake-up interrupt register. */
#define ST25R3916_REG_PT_IRQ              0x1D /**< Passive target interrupt register. */
#define ST25R3916_REG_FIFO_STATUS1        0x1E /**< FIFO status register 1 (count LSB). */
#define ST25R3916_REG_FIFO_STATUS2        0x1F /**< FIFO status register 2 (count MSB + flags). */
#define ST25R3916_REG_COLLISION           0x20 /**< Collision display register. */
#define ST25R3916_REG_PT_STATUS           0x21 /**< Passive target display register. */
#define ST25R3916_REG_NUM_TX_BYTES1       0x22 /**< Number of transmitted bytes register 1. */
#define ST25R3916_REG_NUM_TX_BYTES2       0x23 /**< Number of transmitted bytes register 2. */
#define ST25R3916_REG_BIT_RATE_DET        0x24 /**< Bit rate detection display register. */
#define ST25R3916_REG_AD_RESULT           0x25 /**< A/D converter output register. */
#define ST25R3916_REG_ANT_TUNE_CTRL1      0x26 /**< Antenna tuning control register 1. */
#define ST25R3916_REG_ANT_TUNE_CTRL2      0x27 /**< Antenna tuning control register 2. */
#define ST25R3916_REG_TX_DRIVER           0x28 /**< TX driver register. */
#define ST25R3916_REG_PT_MOD              0x29 /**< Passive target modulation register. */
#define ST25R3916_REG_FIELD_THRESH_ACT    0x2A /**< External field detector activation threshold. */
#define ST25R3916_REG_FIELD_THRESH_DEACT  0x2B /**< External field detector deactivation threshold. */
#define ST25R3916_REG_REGULATOR_CTRL      0x2C /**< Regulator voltage control register. */
#define ST25R3916_REG_RSSI_RESULT         0x2D /**< RSSI display register. */
#define ST25R3916_REG_GAIN_RED_STATE      0x2E /**< Gain reduction state register. */
#define ST25R3916_REG_CAP_SENSOR_CTRL     0x2F /**< Capacitive sensor control register. */
#define ST25R3916_REG_CAP_SENSOR_RESULT   0x30 /**< Capacitive sensor display register. */
#define ST25R3916_REG_AUX_DISPLAY         0x31 /**< Auxiliary display register. */
#define ST25R3916_REG_IC_IDENTITY         0x3F /**< IC identity register. */
#define ST25R3916_SPACE_B                 0x40 /**< Internal marker for Space-B register access. */
#define ST25R3916_REG_EMD_SUP_CONF        (ST25R3916_SPACE_B | 0x05) /**< EMD suppression config. */
#define ST25R3916_REG_CORR_CONF1          (ST25R3916_SPACE_B | 0x0C) /**< Correlator config 1. */
#define ST25R3916_REG_CORR_CONF2          (ST25R3916_SPACE_B | 0x0D) /**< Correlator config 2. */
#define ST25R3916_REG_AUX_MOD             (ST25R3916_SPACE_B | 0x28) /**< Auxiliary modulation setting. */

/* ===== Direct commands (DS12484 Rev 8, Table 13) ===== */
#define ST25R3916_CMD_SET_DEFAULT         0xC1 /**< Set default (power-up state). */
#define ST25R3916_CMD_STOP                0xC2 /**< Stop all activities / clear FIFO. */
#define ST25R3916_CMD_TRANSMIT_WITH_CRC   0xC4 /**< Transmit with automatic CRC. */
#define ST25R3916_CMD_TRANSMIT_WITHOUT_CRC 0xC5 /**< Transmit without CRC. */
#define ST25R3916_CMD_TRANSMIT_REQA       0xC6 /**< Transmit ISO14443-A REQA. */
#define ST25R3916_CMD_TRANSMIT_WUPA       0xC7 /**< Transmit ISO14443-A WUPA. */
#define ST25R3916_CMD_INITIAL_RF_COLLISION 0xC8 /**< NFC initial field on / collision avoidance. */
#define ST25R3916_CMD_RESPONSE_RF_COLLISION_N 0xC9 /**< NFC response field on / collision avoidance. */
#define ST25R3916_CMD_GOTO_SENSE          0xCD /**< Passive target: go to Sense (Idle). */
#define ST25R3916_CMD_GOTO_SLEEP          0xCE /**< Passive target: go to Sleep (Halt). */
#define ST25R3916_CMD_MASK_RECEIVE_DATA   0xD0 /**< Stop receivers / RX decoders. */
#define ST25R3916_CMD_UNMASK_RECEIVE_DATA 0xD1 /**< Start receivers / RX decoders. */
#define ST25R3916_CMD_AM_MOD_STATE_CHANGE 0xD2 /**< Change AM modulation state. */
#define ST25R3916_CMD_MEASURE_AMPLITUDE   0xD3 /**< Measure RF amplitude into A/D output. */
#define ST25R3916_CMD_RESET_RXGAIN        0xD5 /**< Reset RX gain. */
#define ST25R3916_CMD_ADJUST_REGULATORS   0xD6 /**< Adjust supply regulators. */
#define ST25R3916_CMD_CALIBRATE_DRIVER_TIMING 0xD8 /**< Calibrate TX driver timing. */
#define ST25R3916_CMD_MEASURE_PHASE       0xD9 /**< Measure phase difference. */
#define ST25R3916_CMD_CLEAR_RSSI          0xDA /**< Clear RSSI and restart measurement. */
#define ST25R3916_CMD_CLEAR_FIFO          0xDB /**< Clear FIFO. */
#define ST25R3916_CMD_TRANSPARENT_MODE    0xDC /**< Enter transparent (bit-stream) mode. */
#define ST25R3916_CMD_CALIBRATE_C_SENSOR  0xDD /**< Calibrate capacitive sensor. */
#define ST25R3916_CMD_MEASURE_CAPACITANCE 0xDE /**< Measure capacitance. */
#define ST25R3916_CMD_MEASURE_VDD         0xDF /**< Measure power supply. */
#define ST25R3916_CMD_START_GP_TIMER      0xE0 /**< Start general-purpose timer. */
#define ST25R3916_CMD_START_WUP_TIMER     0xE1 /**< Start wake-up timer. */
#define ST25R3916_CMD_START_MASK_RX_TIMER 0xE2 /**< Start mask-receive timer. */
#define ST25R3916_CMD_START_NO_RESPONSE_TIMER 0xE3 /**< Start no-response timer. */
#define ST25R3916_CMD_START_PPON2_TIMER   0xE4 /**< Start PPON2 timer. */
#define ST25R3916_CMD_STOP_NO_RESPONSE_TIMER 0xE8 /**< Stop no-response timer. */
#define ST25R3916_CMD_SPACE_B_ACCESS      0xFB /**< Enable Space-B register access (prefix). */
#define ST25R3916_CMD_TEST_ACCESS         0xFC /**< Enable test register access. */

/* ===== Operation control register (0x02), DS Table 21 ===== */
#define ST25R3916_OP_CONTROL_EN     (1 << 7) /**< en: enable oscillator + regulator. */
#define ST25R3916_OP_CONTROL_RX_EN  (1 << 6) /**< rx_en: enable receiver. */
#define ST25R3916_OP_CONTROL_WU     (1 << 2) /**< wu: enable wake-up mode. */
#define ST25R3916_OP_CONTROL_TX_EN  (1 << 3) /**< tx_en: enable transmitter (RF field). */
#define ST25R3916_OP_CONTROL_EN_FD_AUTO 0x03 /**< en_fd_c=11: automatic external field detector. */
/** Full RF field on for reader/writer: en + rx_en + tx_en. */
#define ST25R3916_OP_CONTROL_FIELD_ON  (ST25R3916_OP_CONTROL_EN | ST25R3916_OP_CONTROL_RX_EN | ST25R3916_OP_CONTROL_TX_EN)
#define ST25R3916_OP_CONTROL_FIELD_OFF 0x00

/* ===== IO configuration register 2 (0x01), DS Table 20 ===== */
#define ST25R3916_IO_CONF2_IO_DRV_LVL (1 << 2) /**< Stronger MISO/IRQ output drivers. */

/* ===== Mode definition register (0x03), DS Table 22/23/24 ===== */
#define ST25R3916_MODE_TARGET       (1 << 7) /**< targ: target/listen mode. */
/* targ = 0 (initiator), om<6:3> selects technology; NFC-A poll/listen = om 0001. */
#define ST25R3916_MODE_INITIATOR    0x00
#define ST25R3916_MODE_OM_NFCA      (0x1 << 3) /**< om = 0001: NFC-A / ISO14443A poll. */
#define ST25R3916_MODE_POLL_NFCA    (ST25R3916_MODE_INITIATOR | ST25R3916_MODE_OM_NFCA)
#define ST25R3916_MODE_TARGET_NFCA  (ST25R3916_MODE_TARGET | ST25R3916_MODE_OM_NFCA)

/* ===== NFCIP-1 passive target definition register (0x08), DS Table 32 ===== */
#define ST25R3916_PT_DEF_FDEL_MASK     0xF0
#define ST25R3916_PT_DEF_DISABLE_AC_A  (1 << 0) /**< 1 disables automatic NFC-A anticollision. */

/* ===== Auxiliary definition register (0x0A), DS Table 36 ===== */
#define ST25R3916_AUX_NFC_ID_MASK      0x30
#define ST25R3916_AUX_NFC_ID_4_BYTES   0x00
#define ST25R3916_AUX_NFC_ID_7_BYTES   0x10

/* ===== ISO14443A and NFC 106 kb/s settings register (0x05), DS Table 28 ===== */
#define ST25R3916_ISO14443A_NO_TX_PAR (1 << 7) /**< Suppress TX parity (for MIFARE/crypto1). */
#define ST25R3916_ISO14443A_NO_RX_PAR (1 << 6) /**< Do not expect RX parity. */
#define ST25R3916_ISO14443A_NFC_F0_CLR (1 << 5)
#define ST25R3916_ISO14443A_ANTCL     (1 << 0) /**< Anti-collision frame (split byte). */

/* ===== Main interrupt register (0x1A), DS Table 62 ===== */
#define ST25R3916_IRQ_MAIN_OSC     (1 << 7) /**< I_osc: oscillator stable. */
#define ST25R3916_IRQ_MAIN_WL      (1 << 6) /**< I_wl: FIFO water level. */
#define ST25R3916_IRQ_MAIN_RXS     (1 << 5) /**< I_rxs: start of receive. */
#define ST25R3916_IRQ_MAIN_RXE     (1 << 4) /**< I_rxe: end of receive. */
#define ST25R3916_IRQ_MAIN_TXE     (1 << 3) /**< I_txe: end of transmission. */
#define ST25R3916_IRQ_MAIN_COL     (1 << 2) /**< I_col: bit collision. */
#define ST25R3916_IRQ_MAIN_RX_REST (1 << 1) /**< I_rx_rest: automatic reception restart. */

/* ===== Timer and NFC interrupt register (0x1B), DS Table 63 ===== */
#define ST25R3916_IRQ_TIMER_DCT (1 << 7) /**< I_dct: direct command terminated. */
#define ST25R3916_IRQ_TIMER_NRE (1 << 6) /**< I_nre: no-response timer expired. */
#define ST25R3916_IRQ_TIMER_GPE (1 << 5) /**< I_gpe: general-purpose timer expired. */
#define ST25R3916_IRQ_TIMER_EON (1 << 4) /**< I_eon: external field on. */
#define ST25R3916_IRQ_TIMER_EOF (1 << 3) /**< I_eof: external field off. */

/* ===== Error and wake-up interrupt register (0x1C), DS Table 64 ===== */
#define ST25R3916_IRQ_ERR_CRC  (1 << 7) /**< I_crc: CRC error. */
#define ST25R3916_IRQ_ERR_PAR  (1 << 6) /**< I_par: parity error. */
#define ST25R3916_IRQ_ERR_HFE  (1 << 5) /**< I_hfe: hard framing error. */
#define ST25R3916_IRQ_ERR_SFE  (1 << 4) /**< I_sfe: soft framing error. */

/* ===== Passive target interrupt register (0x1D), DS Table 65 ===== */
#define ST25R3916_IRQ_PT_RXE_PTA   (1 << 4) /**< Automatic NFC-A/NFC-F response sent. */
#define ST25R3916_IRQ_PT_WU_F      (1 << 3) /**< NFC-F passive target active. */
#define ST25R3916_IRQ_PT_WU_A_STAR (1 << 1) /**< NFC-A Active* reached. */
#define ST25R3916_IRQ_PT_WU_A      (1 << 0) /**< NFC-A Active reached. */

/* ===== FIFO status register 2 (0x1F), DS Table 67 ===== */
#define ST25R3916_FIFO_STATUS2_FTC_MASK 0xC0 /**< Bits [7:6] are the upper bits of the FIFO count. */
#define ST25R3916_FIFO_STATUS2_FTC_SHIFT 6
#define ST25R3916_FIFO_STATUS2_LB_MASK  0x0E /**< Bits [3:1] are the bits in the last FIFO byte. */
#define ST25R3916_FIFO_STATUS2_LB_SHIFT 1

/* ===== Space-B EMD suppression config, correlator, and aux modulation ===== */
#define ST25R3916_EMD_SUP_CONF_RX_START_EMV (1 << 6) /**< Start RX after first 4 bits. */
#define ST25R3916_CORR_CONF1_MOMENTUM       0x51 /**< corr_s0 | corr_s4 | corr_s6. */
#define ST25R3916_AUX_MOD_LM_EXT            (1 << 5) /**< Enable external load modulation. */
#define ST25R3916_AUX_MOD_LM_DRI            (1 << 4) /**< Enable internal load modulation driver. */

/* ===== External field detector threshold registers ===== */
#define ST25R3916_FIELD_THRESH_ACT_MOMENTUM   0x11 /**< 105 mV target/RFE activation thresholds. */
#define ST25R3916_FIELD_THRESH_DEACT_MOMENTUM 0x00 /**< 75 mV target/RFE deactivation thresholds. */

/* ===== Number of transmitted bytes register 2 (0x23), DS Table 71 ===== */
#define ST25R3916_NUM_TX_BYTES2_NTX_BITS_MASK 0x07 /**< [2:0] partial-byte bit count. */

/* ===== Passive target display register (0x21), DS Table 69 ===== */
#define ST25R3916_PT_STATUS_STATE_MASK 0x0F
#define ST25R3916_PT_STATUS_POWER_OFF  0x00
#define ST25R3916_PT_STATUS_IDLE       0x01
#define ST25R3916_PT_STATUS_READY_L1   0x02
#define ST25R3916_PT_STATUS_READY_L2   0x03
#define ST25R3916_PT_STATUS_ACTIVE     0x05
#define ST25R3916_PT_STATUS_HALT       0x09
#define ST25R3916_PT_STATUS_READY_L1_STAR 0x0A
#define ST25R3916_PT_STATUS_READY_L2_STAR 0x0B
#define ST25R3916_PT_STATUS_ACTIVE_STAR 0x0D

/* ===== TX driver register (0x28), DS Table 79 ===== */
#define ST25R3916_TX_DRIVER_LM_DRI (1 << 4) /**< Enable internal driver load modulation. */

/* ===== Passive target modulation register (0x29), DS Table 80 ===== */
#define ST25R3916_PT_MOD_INTERNAL_LM 0x0F /**< ptm_res=0 (1.0Ω strong), pt_res=15 (High-Z non-mod). */

/* ===== IC identity register (0x3F), DS Table 117 ===== */
#define ST25R3916_IC_TYPE_SHIFT 3    /**< IC type in bits [7:3]. */
#define ST25R3916_IC_TYPE_MASK  0x1F
#define ST25R3916_IC_REV_MASK   0x07 /**< IC revision in bits [2:0]. */

/* ST25R3916 FIFO capacity (bytes). */
#define ST25R3916_FIFO_DEPTH 512

#ifdef __cplusplus
}
#endif

#endif // ST25R3916_REG_H
