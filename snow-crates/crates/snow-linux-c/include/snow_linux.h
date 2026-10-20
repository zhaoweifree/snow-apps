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

/* Global keyboard shortcuts, over XGrabKey.
 *
 * A grab takes the key combination away from every other client for as long as
 * this handle lives. That is what makes a screenshot hotkey work while another
 * application has focus, and it is also why a binding must never be a bare
 * modifier key: the Rust side refuses those instead of registering them.
 */
typedef struct SnowLinuxHotkeys SnowLinuxHotkeys;

/* Called on an internal worker thread, never on the caller's thread, with the
 * registration id passed to snow_linux_hotkeys_register().
 *
 * The handler must not call back into this handle: registration waits for that
 * same worker to answer, so a call from inside the handler would wait for
 * itself. Post to the application thread instead.
 */
typedef void (*SnowLinuxHotkeyHandler)(int32_t registration_id, void *user_data);

/* Modifier bits for snow_linux_hotkeys_register(). These are not X11 masks:
 * which X11 modifiers Alt and Super occupy depends on the layout, and resolving
 * that is the Rust side's job.
 */
#define SNOW_LINUX_MODIFIER_CONTROL 1u
#define SNOW_LINUX_MODIFIER_ALT 2u
#define SNOW_LINUX_MODIFIER_SHIFT 4u
#define SNOW_LINUX_MODIFIER_META 8u

/* snow_linux_hotkeys_register() results. */
#define SNOW_LINUX_HOTKEY_OK 0
/* The key is not on this keyboard, so there is nothing to grab. */
#define SNOW_LINUX_HOTKEY_KEY_NOT_ON_KEYBOARD 1
/* Another client -- or another registration in this handle -- holds it. */
#define SNOW_LINUX_HOTKEY_ALREADY_IN_USE 2
/* The server refused for another reason, or did not answer in time. */
#define SNOW_LINUX_HOTKEY_SERVER_ERROR 3
/* The Qt key or the modifier bits do not name a bindable combination. */
#define SNOW_LINUX_HOTKEY_INVALID_BINDING 4

/* Opens a hotkey service on a connection of its own.
 *
 * Returns NULL when no X server is reachable, or when the keyboard tables cannot
 * be read. `handler` may not be NULL.
 */
SnowLinuxHotkeys *snow_linux_hotkeys_create(SnowLinuxHotkeyHandler handler, void *user_data);

/* Releases every grab and joins the worker. */
void snow_linux_hotkeys_destroy(SnowLinuxHotkeys *hotkeys);

/* Whether a Qt key and modifier bits name a bindable combination, without
 * contacting the server: SNOW_LINUX_HOTKEY_OK or SNOW_LINUX_HOTKEY_INVALID_BINDING.
 *
 * This is what the settings UI asks while the user records a shortcut. A result
 * of OK does not promise the grab will succeed -- the key may simply not be on
 * this keyboard -- so registration's own result is still the one to report.
 */
int32_t snow_linux_hotkeys_validate_binding(int32_t qt_key, uint32_t modifier_bits);

/* Grabs `qt_key` with `modifier_bits`.
 *
 * `qt_key` is the Qt::Key value, which is stable between Qt 5 and Qt 6 for
 * everything but two media keys; both spellings of those are accepted. Returns
 * one of the SNOW_LINUX_HOTKEY_* codes. Re-registering an id replaces it.
 */
int32_t snow_linux_hotkeys_register(SnowLinuxHotkeys *hotkeys, int32_t registration_id, int32_t qt_key,
                                    uint32_t modifier_bits);

/* Releases the grabs held for `registration_id`. Unknown ids are ignored. */
void snow_linux_hotkeys_unregister(SnowLinuxHotkeys *hotkeys, int32_t registration_id);

/* Whether a key producing `qt_key` is held right now: 1 down, 0 up, -1 unknown.
 *
 * -1 means the worker did not answer in time. Callers must treat it as "not
 * observable" rather than "not pressed": a shortcut picker that reads it as "up"
 * would accept a half-finished combination.
 */
int32_t snow_linux_hotkeys_key_down(const SnowLinuxHotkeys *hotkeys, int32_t qt_key);

#ifdef __cplusplus
}
#endif
