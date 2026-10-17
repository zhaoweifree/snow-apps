#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* X11 platform services for the Qt application.
 *
 * Every call here is best-effort: the return value says whether the answer is
 * meaningful, never why it is not. All failures share one cause -- no reachable
 * X server, no platform handle, or a pointer on another root -- and the caller
 * has nothing different to do about any of them.
 */

typedef struct SnowLinuxPlatform SnowLinuxPlatform;

/* Opens the platform, keeping one process-wide X11 connection.
 *
 * Returns NULL when no X server is reachable: a Wayland-only session without
 * XWayland, or a display that refused the connection. The first attempt decides
 * for the life of the process.
 */
SnowLinuxPlatform *snow_linux_platform_create(void);

/* Releases the handle. The connection itself stays open for other holders. */
void snow_linux_platform_destroy(SnowLinuxPlatform *platform);

/* Pointer position in root-window pixels, which are device pixels.
 *
 * Qt's logical coordinates differ from these when the session scales; converting
 * them is the caller's job because the conversion needs the screen. Returns 1 on
 * success and 0 when no position is available.
 */
int snow_linux_platform_pointer_position(const SnowLinuxPlatform *platform, int32_t *x,
                                         int32_t *y);

/* Moves the pointer to an absolute root position.
 *
 * Returns 1 when the request was accepted and flushed to the server. Returns 0
 * for a position outside the signed 16-bit root coordinate space rather than
 * clamping it: an out-of-range request is a caller bug and moving the cursor to
 * the nearest edge would hide it.
 */
int snow_linux_platform_set_pointer_position(SnowLinuxPlatform *platform, int32_t x, int32_t y);

#ifdef __cplusplus
}
#endif
