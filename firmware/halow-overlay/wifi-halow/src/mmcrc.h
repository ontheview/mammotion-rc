/*
 * Copyright 2024 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Path C minimal mmcrc.h — the original wasn't shipped in the include/
 * directory of mm-iot-esp32 2.8.2.  Reconstructed from the single function
 * defined in mmcrc.c (only export needed).
 */

#ifndef MMCRC_H_
#define MMCRC_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CRC-16/XMODEM (poly 0x1021, init 0x0000, no reflect, xor 0x0000).
 * Pass the prior crc as the first argument (or 0 for a fresh sum). */
uint16_t mmcrc_16_xmodem(uint16_t crc, const void *data, size_t data_len);

#ifdef __cplusplus
}
#endif

#endif /* MMCRC_H_ */
