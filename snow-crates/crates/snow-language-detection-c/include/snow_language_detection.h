#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnowLanguageDetectionResult {
    /* NUL-terminated ISO 639-3 code. Mandarin is "cmn" for either Chinese script. */
    char language[4];
    /* Raw whatlang confidence in [0, 1]; no reliability cutoff is applied. */
    double confidence;
} SnowLanguageDetectionResult;

/* Detect all input together with one whatlang call, without splitting or truncating.
 * Returns 1 for a detection, 0 for no detection, and -1 for invalid arguments or UTF-8.
 * A non-null output is cleared before processing and must point to a writable,
 * aligned result that does not overlap input. Input must cover length readable bytes
 * and remain alive for this call. Null input with zero length is an empty input.
 * No returned data requires destruction. */
int32_t snow_language_detect(const uint8_t* input, size_t length,
                             SnowLanguageDetectionResult* output);

#ifdef __cplusplus
}
#endif
