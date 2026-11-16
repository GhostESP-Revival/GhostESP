/**
 * @file deauth_pmf_note.h
 * @brief Plain-English note about whether deauth is expected to get through
 *
 * Lives apart from the rest of the WPA3 compliance reporting so the message
 * selection can be unit tested without dragging in the AP scan, display and
 * logging dependencies. The authmode is taken as a plain uint8_t because that
 * is how a station record stores the security type of the AP it was seen on.
 */
#ifndef DEAUTH_PMF_NOTE_H
#define DEAUTH_PMF_NOTE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Describe, in one sentence, how likely deauth is to work on a network
 *        with this security type.
 *
 * Returns NULL when there is nothing useful to add: an open or WEP network has
 * no management frame protection, so deauth is expected to work and no note is
 * warranted. Never returns an empty string, and never contains a newline, so
 * the caller can print it as a single line.
 */
const char *deauth_pmf_note(uint8_t authmode);

#ifdef __cplusplus
}
#endif

#endif /* DEAUTH_PMF_NOTE_H */
