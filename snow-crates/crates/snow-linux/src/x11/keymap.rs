//! The server's keyboard description, read once and reused by every grab.
//!
//! Two features need the same three tables: which keycodes produce a keysym
//! (`XGrabKey`), which modifier bit Alt and Super occupy (both grabs), and which
//! locks this keyboard has (both grabs, because X11 matching is exact and a grab
//! that ignores CapsLock stops working the moment it is on).
//!
//! Reading them is two round-trips, so they live here rather than being fetched
//! per caller: the hotkey service and the mouse-drag service each own a
//! connection and each read this once at start.

use x11rb::connection::Connection;
use x11rb::protocol::xproto::ConnectionExt as _;
use x11rb::rust_connection::RustConnection;

use crate::error::LinuxResult;
use crate::hotkey::{self, HotkeyModifiers};

/// Every modifier bit X11 defines. The high bits of `state` are buttons, which
/// must not take part in matching a passive grab of either kind.
pub const MODIFIER_STATE_MASK: u16 = 0x00ff;

/// Lock (CapsLock). The protocol fixes this one; NumLock and ScrollLock do not
/// have fixed masks and are read back from the server.
const LOCK_MASK: u16 = 0x0002;

/// Control, also fixed by the protocol.
const CONTROL_MASK: u16 = 0x0004;

/// Shift, which is required when a binding's keysym only lives on the shifted
/// level of its keycode.
pub const SHIFT_MASK: u16 = 0x0001;

/// `Mod1` and `Mod4`, the conventional homes of Alt and Super. Used only when
/// the keyboard mapping does not identify them, which is the case for a layout
/// with no Alt or Super key at all.
const FALLBACK_ALT_MASK: u16 = 0x0008;
const FALLBACK_META_MASK: u16 = 0x0040;

const KEYSYM_ALT_L: u32 = 0xffe9;
const KEYSYM_ALT_R: u32 = 0xffea;
const KEYSYM_META_L: u32 = 0xffe7;
const KEYSYM_META_R: u32 = 0xffe8;
const KEYSYM_SUPER_L: u32 = 0xffeb;
const KEYSYM_SUPER_R: u32 = 0xffec;
const KEYSYM_HYPER_L: u32 = 0xffed;
const KEYSYM_HYPER_R: u32 = 0xffee;
const KEYSYM_NUM_LOCK: u32 = 0xff7f;
const KEYSYM_SCROLL_LOCK: u32 = 0xff14;

/// The parts of the keyboard description that turn a binding into grabs.
pub struct Keymap {
    pub(crate) min_keycode: u8,
    pub(crate) per_keycode: usize,
    pub(crate) keysyms: Vec<u32>,
    pub(crate) alt_mask: u16,
    pub(crate) meta_mask: u16,
    /// The lock modifiers this keyboard actually has, so a grab survives
    /// CapsLock / NumLock / ScrollLock being on.
    pub(crate) lock_masks: Vec<u16>,
}

impl Keymap {
    pub fn read(conn: &RustConnection) -> LinuxResult<Self> {
        let setup = conn.setup();
        let min_keycode = setup.min_keycode;
        // `count` is one byte wide in the protocol: the whole keycode space is
        // 8 bits, so the inclusive span always fits.
        let count = setup.max_keycode - setup.min_keycode + 1;
        let keyboard = conn
            .get_keyboard_mapping(min_keycode, count)
            .map_err(|err| crate::error::LinuxError::backend("reading the keyboard mapping", err))?
            .reply()
            .map_err(|err| crate::error::LinuxError::reply("reading the keyboard mapping", err))?;
        let modifier = conn
            .get_modifier_mapping()
            .map_err(|err| crate::error::LinuxError::backend("reading the modifier mapping", err))?
            .reply()
            .map_err(|err| crate::error::LinuxError::reply("reading the modifier mapping", err))?;

        let per_keycode = usize::from(keyboard.keysyms_per_keycode);
        // The modifier map is one flat array of keycodes, `keycodes_per_modifier`
        // per modifier, in mask order. A zero width would mean the server has no
        // modifier keys at all, which is legal but useless here.
        let mut modifier_map: Vec<Vec<u8>> = if modifier.keycodes_per_modifier() == 0 {
            Vec::new()
        } else {
            modifier
                .keycodes
                .chunks(usize::from(modifier.keycodes_per_modifier()))
                .map(<[u8]>::to_vec)
                .collect()
        };
        modifier_map.resize(8, Vec::new());

        let mask_for = |wanted: &[u32]| {
            hotkey::modifier_mask_for(
                &modifier_map,
                min_keycode,
                per_keycode,
                &keyboard.keysyms,
                wanted,
            )
        };
        // Alt is Mod1 on every mainstream layout, but "on every mainstream
        // layout" is an assumption, and the cost of checking is one reply that
        // is needed anyway for the keyboard mapping.
        let alt_mask = mask_for(&[KEYSYM_ALT_L, KEYSYM_ALT_R])
            .or_else(|| mask_for(&[KEYSYM_META_L, KEYSYM_META_R]))
            .unwrap_or(FALLBACK_ALT_MASK);
        let meta_mask = mask_for(&[KEYSYM_SUPER_L, KEYSYM_SUPER_R])
            .or_else(|| mask_for(&[KEYSYM_HYPER_L, KEYSYM_HYPER_R]))
            .unwrap_or(FALLBACK_META_MASK);

        let mut lock_masks = vec![LOCK_MASK];
        for keysym in [KEYSYM_NUM_LOCK, KEYSYM_SCROLL_LOCK] {
            if let Some(mask) = mask_for(&[keysym]) {
                // A "lock" that is really the Alt or Super modifier would turn
                // every Alt binding into a grab of Alt plus itself; skip it and
                // accept that hotkeys will miss while that key is held.
                if mask != alt_mask && mask != meta_mask && !lock_masks.contains(&mask) {
                    lock_masks.push(mask);
                }
            }
        }

        Ok(Self {
            min_keycode,
            per_keycode,
            keysyms: keyboard.keysyms,
            alt_mask,
            meta_mask,
            lock_masks,
        })
    }

    pub fn mask(&self, modifiers: HotkeyModifiers) -> u16 {
        self.mask_bits(modifiers.bits())
    }

    /// The same translation for callers that carry the bits directly.
    ///
    /// The mouse-drag service takes modifier flags through the C ABI, where they
    /// are plain bits; going through `HotkeyModifiers` only to read them back
    /// would hide a validation the caller has already done.
    pub fn mask_bits(&self, bits: u32) -> u16 {
        let mut mask = 0;
        if bits & hotkey::MODIFIER_CONTROL != 0 {
            mask |= CONTROL_MASK;
        }
        if bits & hotkey::MODIFIER_SHIFT != 0 {
            mask |= SHIFT_MASK;
        }
        if bits & hotkey::MODIFIER_ALT != 0 {
            mask |= self.alt_mask;
        }
        if bits & hotkey::MODIFIER_META != 0 {
            mask |= self.meta_mask;
        }
        mask
    }

    pub fn lock_masks(&self) -> &[u16] {
        &self.lock_masks
    }

    pub fn keycode_matches(&self, keysym: u32) -> Vec<hotkey::KeycodeMatch> {
        hotkey::keycode_matches(keysym, self.min_keycode, self.per_keycode, &self.keysyms)
    }

    /// Whether any keycode currently producing `keysym` is held down.
    pub fn is_down(&self, conn: &RustConnection, keysym: u32) -> Option<bool> {
        let reply = conn.query_keymap().ok()?.reply().ok()?;
        Self::pressed_in(&reply.keys, &self.keycode_matches(keysym))
    }

    fn pressed_in(bitmap: &[u8], matches: &[hotkey::KeycodeMatch]) -> Option<bool> {
        if bitmap.is_empty() {
            return None;
        }
        for item in matches {
            let index = usize::from(item.keycode);
            let byte = bitmap.get(index / 8)?;
            if (byte >> (index % 8)) & 1 != 0 {
                return Some(true);
            }
        }
        Some(false)
    }
}
