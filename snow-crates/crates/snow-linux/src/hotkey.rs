//! Global shortcut bindings, translated from Qt key codes into X11 terms.
//!
//! Everything here is pure: it turns a Qt key plus a set of modifier flags into
//! the keysym and modifier masks that `XGrabKey` needs, and decides which
//! keycodes carry that keysym. Nothing in this module talks to a server, which
//! is what lets the rules be unit-tested on a machine with no display at all --
//! the part of a hotkey backend that is easiest to get subtly wrong is the
//! translation, not the plumbing.
//!
//! # Why not ask Qt for the keysym
//!
//! Qt has a keysym-to-Qt-key table (its X11 plugin uses one) but no public
//! inverse. The Qt key code is what the C side already has, so the mapping lives
//! here, spelled out and testable, instead of being reconstructed from private
//! Qt internals on the C++ side.

/// Control is held. Part of the C ABI: do not renumber.
pub const MODIFIER_CONTROL: u32 = 1 << 0;
/// Alt (X11 `Mod1`, usually) is held. Part of the C ABI: do not renumber.
pub const MODIFIER_ALT: u32 = 1 << 1;
/// Shift is held. Part of the C ABI: do not renumber.
pub const MODIFIER_SHIFT: u32 = 1 << 2;
/// Super / Meta is held. Part of the C ABI: do not renumber.
pub const MODIFIER_META: u32 = 1 << 3;

/// Every bit [`HotkeyModifiers`] understands. Anything else is a caller bug.
pub const KNOWN_MODIFIERS: u32 = MODIFIER_CONTROL | MODIFIER_ALT | MODIFIER_SHIFT | MODIFIER_META;

/// The modifier half of a binding.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct HotkeyModifiers {
    bits: u32,
}

impl HotkeyModifiers {
    pub const NONE: Self = Self { bits: 0 };

    /// Returns `None` for bits that do not name a modifier.
    ///
    /// Ignoring unknown bits would turn a caller's typo into a hotkey that is
    /// registered one modifier short and fires too eagerly.
    pub const fn from_bits(bits: u32) -> Option<Self> {
        if bits & !KNOWN_MODIFIERS != 0 {
            return None;
        }
        Some(Self { bits })
    }

    pub const fn bits(self) -> u32 {
        self.bits
    }

    pub const fn contains(self, flag: u32) -> bool {
        self.bits & flag == flag
    }

    pub const fn is_empty(self) -> bool {
        self.bits == 0
    }
}

/// A binding resolved against X11's keysym space.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Hotkey {
    pub keysym: u32,
    pub modifiers: HotkeyModifiers,
}

/// Why a Qt key plus modifier flags cannot become a hotkey.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HotkeyError {
    /// The modifier flags contain a bit outside [`KNOWN_MODIFIERS`].
    UnknownModifier,
    /// The Qt key is a modifier key held on its own: a grab of it would swallow
    /// that modifier for the whole session, which is never what a user means.
    ModifierOnlyKey,
    /// No X11 keysym corresponds to this Qt key, so there is nothing to grab.
    UnsupportedKey,
}

/// The keysym for a Qt key code, or `None` when X11 has no equivalent.
///
/// The printable range is the identity: X11's Latin-1 keysyms were defined to
/// coincide with ASCII, and Qt's printable keys use ASCII values too, so both
/// sides agree without a table. Everything else is listed explicitly. The Qt
/// side was read out of the Qt 5.15.2 and 6.12.0 `qnamespace.h` headers, which
/// agree on every value used here except the two media keys noted at the bottom
/// of the table; the X11 side comes from xorgproto's `keysymdef.h` and
/// `XF86keysym.h`, not from memory.
pub fn keysym_for_qt_key(qt_key: u32) -> Option<u32> {
    if (0x20..=0x7e).contains(&qt_key) {
        return Some(qt_key);
    }
    // F1..F35 are contiguous on both sides: Qt starts at 0x01000030, X11 at
    // 0xffbe (XK_F1), and XK_F35 is 0xffe0.
    if (0x0100_0030..=0x0100_0052).contains(&qt_key) {
        return Some(0xffbe + (qt_key - 0x0100_0030));
    }
    // Launch0..LaunchF are contiguous too: XF86XK_Launch0 is 0x1008ff40.
    if (0x0100_00a2..=0x0100_00b1).contains(&qt_key) {
        return Some(0x1008_ff40 + (qt_key - 0x0100_00a2));
    }
    let keysym = match qt_key {
        0x0100_0000 => 0xff1b, // Escape
        0x0100_0001 => 0xff09, // Tab
        // Backtab is Shift+Tab on the same physical key, and the Windows
        // backend maps it to VK_TAB as well rather than inventing a key.
        0x0100_0002 => 0xff09,
        0x0100_0003 => 0xff08, // Backspace
        0x0100_0004 => 0xff0d, // Return
        0x0100_0005 => 0xff8d, // Enter (keypad)
        0x0100_0006 => 0xff63, // Insert
        0x0100_0007 => 0xffff, // Delete
        0x0100_0008 => 0xff13, // Pause
        0x0100_0009 => 0xff61, // Print (PrintScreen)
        0x0100_000a => 0xff15, // SysReq
        0x0100_000b => 0xff0b, // Clear
        0x0100_0010 => 0xff50, // Home
        0x0100_0011 => 0xff57, // End
        0x0100_0012 => 0xff51, // Left
        0x0100_0013 => 0xff52, // Up
        0x0100_0014 => 0xff53, // Right
        0x0100_0015 => 0xff54, // Down
        0x0100_0016 => 0xff55, // PageUp (XK_Prior)
        0x0100_0017 => 0xff56, // PageDown (XK_Next)
        // Modifier keys. Rejected as *bindings* by [`hotkey`], but their keysyms
        // are still needed to answer "is this key down" for `inputState`.
        0x0100_0020 => 0xffe1, // Shift
        0x0100_0021 => 0xffe3, // Control
        0x0100_0022 => 0xffe7, // Meta
        0x0100_0023 => 0xffe9, // Alt
        0x0100_0024 => 0xffe5, // CapsLock
        0x0100_0025 => 0xff7f, // NumLock
        0x0100_0026 => 0xff14, // ScrollLock
        0x0100_0053 => 0xffeb, // Super_L
        0x0100_0054 => 0xffec, // Super_R
        0x0100_0055 => 0xff67, // Menu
        0x0100_0056 => 0xffed, // Hyper_L
        0x0100_0057 => 0xffee, // Hyper_R
        0x0100_0058 => 0xff6a, // Help
        // Browser keys (XF86*).
        0x0100_0061 => 0x1008_ff26, // Back
        0x0100_0062 => 0x1008_ff27, // Forward
        0x0100_0063 => 0x1008_ff28, // Stop
        0x0100_0064 => 0x1008_ff29, // Refresh
        0x0100_0092 => 0x1008_ff1b, // Search
        0x0100_0090 => 0x1008_ff18, // HomePage
        0x0100_0091 => 0x1008_ff30, // Favorites
        // Media keys.
        0x0100_0070 => 0x1008_ff11, // VolumeDown
        0x0100_0071 => 0x1008_ff12, // VolumeMute
        0x0100_0072 => 0x1008_ff13, // VolumeUp
        0x0100_0080 => 0x1008_ff14, // MediaPlay
        0x0100_0081 => 0x1008_ff15, // MediaStop
        0x0100_0082 => 0x1008_ff16, // MediaPrevious
        0x0100_0083 => 0x1008_ff17, // MediaNext
        0x0100_0084 => 0x1008_ff1c, // MediaRecord (XF86XK_AudioRecord)
        // Qt 5.15 spells these two with a missing zero (0x1000085, 0x1000086);
        // Qt 6 corrected it to 0x01000085/86. Both spellings are accepted so the
        // same table serves either Qt.
        0x1000_0085 | 0x0100_0085 => 0x1008_ff31, // MediaPause
        0x1000_0086 | 0x0100_0086 => 0x1008_ff31, // MediaTogglePlayPause
        0x0100_0093 => 0x1008_ff10,               // Standby
        0x0100_00a0 => 0x1008_ff19,               // LaunchMail
        0x0100_00a1 => 0x1008_ff32,               // LaunchMedia (XF86XK_AudioMedia)
        0x0100_00b2 => 0x1008_ff02,               // MonBrightnessUp
        0x0100_00b3 => 0x1008_ff03,               // MonBrightnessDown
        0x0100_00b7 => 0x1008_ff2a,               // PowerOff
        0x0100_00b8 => 0x1008_ff2b,               // WakeUp
        0x0100_00b9 => 0x1008_ff2c,               // Eject
        0x0100_00ba => 0x1008_ff2d,               // ScreenSaver
        0x0100_00bb => 0x1008_ff2e,               // WWW
        0x0100_00cb => 0x1008_ff1d,               // Calculator
        _ => return None,
    };
    Some(keysym)
}

/// Qt keys that are modifiers and therefore cannot be a binding on their own.
const MODIFIER_KEYS: [u32; 9] = [
    0x0100_0020, // Shift
    0x0100_0021, // Control
    0x0100_0022, // Meta
    0x0100_0023, // Alt
    0x0100_0053, // Super_L
    0x0100_0054, // Super_R
    0x0100_0056, // Hyper_L
    0x0100_0057, // Hyper_R
    0x0100_0024, // CapsLock
];

/// Resolves a Qt key and modifier flags into the binding to grab.
///
/// A modifier-only binding is refused rather than registered: `XGrabKey` on
/// Control or Shift alone would take that modifier away from every application
/// on the session, and no amount of care on the C++ side can make that
/// reversible for the user.
pub fn hotkey(qt_key: u32, modifier_bits: u32) -> Result<Hotkey, HotkeyError> {
    let modifiers =
        HotkeyModifiers::from_bits(modifier_bits).ok_or(HotkeyError::UnknownModifier)?;
    if MODIFIER_KEYS.contains(&qt_key) {
        return Err(HotkeyError::ModifierOnlyKey);
    }
    let keysym = keysym_for_qt_key(qt_key).ok_or(HotkeyError::UnsupportedKey)?;
    Ok(Hotkey { keysym, modifiers })
}

/// The keysyms worth looking for, in preference order.
///
/// Qt reports an uppercase key for both `a` and `A`, but the X keyboard mapping
/// stores the lowercase keysym on the unshifted level. Looking only for
/// `XK_A` would find the shifted level and conclude that every letter needs
/// Shift, so binding `Ctrl+A` would silently become `Ctrl+Shift+A`. Folding to
/// the lowercase keysym is what makes the unshifted level findable again.
fn keysym_alternatives(keysym: u32) -> [Option<u32>; 2] {
    let folded = match keysym {
        0x41..=0x5a => Some(keysym + 0x20),
        _ => None,
    };
    [Some(keysym), folded]
}

/// One physical key that produces a keysym, and whether reaching that keysym
/// needs Shift to be held.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct KeycodeMatch {
    pub keycode: u8,
    /// The keysym sits on the shifted level of this keycode, so the grab has to
    /// include Shift or the combination will never fire.
    pub implied_shift: bool,
}

/// Finds the keycodes that can produce `keysym`.
///
/// `keysyms` is the flat array from `GetKeyboardMapping`: `per_keycode` entries
/// per keycode, starting at `min_keycode`. Only the first two entries (the
/// unshifted and shifted levels of the first group) are considered, because
/// those are the levels a plain or Shift-modified press reaches; the further
/// levels belong to layouts that need a modifier this backend does not model.
///
/// Unshifted matches win outright: if any keycode produces the keysym without
/// Shift, the shifted ones are dropped rather than also grabbed, since grabbing
/// both would make one binding fire from two different key combinations.
pub fn keycode_matches(
    keysym: u32,
    min_keycode: u8,
    per_keycode: usize,
    keysyms: &[u32],
) -> Vec<KeycodeMatch> {
    if per_keycode == 0 {
        return Vec::new();
    }
    let alternatives = keysym_alternatives(keysym);
    let matches_any = |value: u32| alternatives.iter().flatten().any(|&it| it == value);

    let mut unshifted = Vec::new();
    let mut shifted = Vec::new();
    for (index, group) in keysyms.chunks(per_keycode).enumerate() {
        // 0 is `NoSymbol`: the keycode exists but is not on this layout.
        if group[0] == 0 {
            continue;
        }
        let Ok(keycode) = u8::try_from(usize::from(min_keycode) + index) else {
            continue;
        };
        if matches_any(group[0]) {
            unshifted.push(KeycodeMatch {
                keycode,
                implied_shift: false,
            });
        } else if group.get(1).is_some_and(|&it| matches_any(it)) {
            shifted.push(KeycodeMatch {
                keycode,
                implied_shift: true,
            });
        }
    }
    if unshifted.is_empty() {
        shifted
    } else {
        unshifted
    }
}

/// Every modifier combination a grab has to cover for `base`.
///
/// `XGrabKey` matches the modifier state exactly, so a grab that names only
/// Control stops working the moment CapsLock or NumLock is on. Each lock
/// modifier that is actually present on this keyboard is OR-ed in, giving at
/// most eight grabs per binding.
pub fn grab_masks(base: u16, lock_masks: &[u16]) -> Vec<u16> {
    let mut masks = vec![base];
    for &lock in lock_masks {
        if lock == 0 {
            continue;
        }
        let mut extended = Vec::with_capacity(masks.len() * 2);
        for &mask in &masks {
            let with_lock = mask | lock;
            if with_lock != mask {
                extended.push(with_lock);
            }
        }
        masks.extend(extended);
    }
    masks.sort_unstable();
    masks.dedup();
    masks
}

/// The modifier mask that holds `wanted` keysyms, from the server's tables.
///
/// X11 does not fix what Alt and Super are: the protocol defines `Mod1`..`Mod5`
/// and leaves their meaning to the layout, and a real keyboard map may put Alt
/// on `Mod1` (usual), Super on `Mod4` (usual) or something else entirely.
/// Reading it back is the only way to grab what the user's modifier actually
/// sets.
///
/// `modifier_map` is the eight per-modifier keycode lists from
/// `GetModifierMapping`, in mask order: index 0 is Shift, 1 is Lock, 2 is
/// Control, and 3..7 are `Mod1`..`Mod5`, i.e. `1 << index` is that entry's mask.
/// `keyboard` is the keysym table in the same shape [`keycode_matches`] takes.
pub fn modifier_mask_for(
    modifier_map: &[Vec<u8>],
    min_keycode: u8,
    per_keycode: usize,
    keyboard: &[u32],
    wanted: &[u32],
) -> Option<u16> {
    if per_keycode == 0 {
        return None;
    }
    for (index, keycodes) in modifier_map.iter().enumerate().take(8) {
        // Mask order is Shift, Lock, Control, Mod1..Mod5, so the mask is the
        // index's bit. Getting this off by one would grab Control instead of
        // Alt, which registers cleanly and then never fires.
        let mask = 1u16 << index;
        for &keycode in keycodes {
            let Some(offset) = keycode.checked_sub(min_keycode) else {
                continue;
            };
            let start = usize::from(offset) * per_keycode;
            let Some(group) = keyboard.get(start..start + per_keycode) else {
                continue;
            };
            if group.iter().any(|value| wanted.contains(value)) {
                return Some(mask);
            }
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn printable_keys_map_to_their_own_code() {
        // 'A' is 0x41 on both sides; X11 named that keysym XK_A.
        assert_eq!(keysym_for_qt_key(0x41), Some(0x41));
        assert_eq!(keysym_for_qt_key(0x31), Some(0x31));
        assert_eq!(keysym_for_qt_key(0x20), Some(0x20));
    }

    #[test]
    fn function_keys_are_contiguous_on_both_sides() {
        assert_eq!(keysym_for_qt_key(0x0100_0030), Some(0xffbe)); // F1
        assert_eq!(keysym_for_qt_key(0x0100_0037), Some(0xffc5)); // F8
        assert_eq!(keysym_for_qt_key(0x0100_0047), Some(0xffd5)); // F24
        assert_eq!(keysym_for_qt_key(0x0100_0052), Some(0xffe0)); // F35
        assert_eq!(keysym_for_qt_key(0x0100_0053), Some(0xffeb)); // Super_L
    }

    #[test]
    fn named_keys_have_their_x11_values() {
        assert_eq!(keysym_for_qt_key(0x0100_0000), Some(0xff1b));
        assert_eq!(keysym_for_qt_key(0x0100_0004), Some(0xff0d));
        assert_eq!(keysym_for_qt_key(0x0100_0016), Some(0xff55));
        assert_eq!(keysym_for_qt_key(0x0100_0009), Some(0xff61));
        assert_eq!(keysym_for_qt_key(0x0100_0058), Some(0xff6a)); // Help
        assert_eq!(keysym_for_qt_key(0x0100_0056), Some(0xffed)); // Hyper_L
        assert_eq!(keysym_for_qt_key(0x0100_0057), Some(0xffee)); // Hyper_R
    }

    #[test]
    fn the_launch_keys_are_contiguous_too() {
        assert_eq!(keysym_for_qt_key(0x0100_00a2), Some(0x1008_ff40)); // Launch0
        assert_eq!(keysym_for_qt_key(0x0100_00b1), Some(0x1008_ff4f)); // LaunchF
    }

    #[test]
    fn media_keys_carry_their_own_keysym() {
        // AudioRecord, not Mail: 0x1008ff19 is XF86Mail, and a recording hotkey
        // that fires the mail client would look like a symptom of something
        // else entirely.
        assert_eq!(keysym_for_qt_key(0x0100_0084), Some(0x1008_ff1c));
        assert_eq!(keysym_for_qt_key(0x0100_00a0), Some(0x1008_ff19));
        assert_eq!(keysym_for_qt_key(0x0100_00b7), Some(0x1008_ff2a)); // PowerOff
        assert_eq!(keysym_for_qt_key(0x0100_00b9), Some(0x1008_ff2c)); // Eject
    }

    #[test]
    fn both_spellings_of_the_media_pause_key_are_accepted() {
        // Qt 5.15 wrote 0x1000085 (one zero short); Qt 6 fixed it. A Qt5 build
        // is what ships on Linux, so both have to resolve to the same keysym.
        assert_eq!(keysym_for_qt_key(0x1000_0085), Some(0x1008_ff31));
        assert_eq!(keysym_for_qt_key(0x0100_0085), Some(0x1008_ff31));
    }

    #[test]
    fn an_unmapped_key_is_refused() {
        assert_eq!(keysym_for_qt_key(0x0100_00ff), None);
        assert_eq!(keysym_for_qt_key(0x0fff_0000), None);
    }

    #[test]
    fn a_modifier_alone_is_refused() {
        assert_eq!(hotkey(0x0100_0021, 0), Err(HotkeyError::ModifierOnlyKey));
        assert_eq!(
            hotkey(0x0100_0020, MODIFIER_SHIFT),
            Err(HotkeyError::ModifierOnlyKey)
        );
    }

    #[test]
    fn unknown_modifier_bits_are_refused() {
        assert_eq!(
            hotkey(0x0100_0030, 1 << 7),
            Err(HotkeyError::UnknownModifier)
        );
    }

    #[test]
    fn a_normal_binding_resolves() {
        let binding = hotkey(0x0100_0030, MODIFIER_CONTROL | MODIFIER_SHIFT).unwrap();
        assert_eq!(binding.keysym, 0xffbe);
        assert!(binding.modifiers.contains(MODIFIER_CONTROL));
        assert!(binding.modifiers.contains(MODIFIER_SHIFT));
        assert!(!binding.modifiers.contains(MODIFIER_ALT));
    }

    /// A keyboard where 'a' is keycode 38, '1' is 10, '=' is 21 with '+' on the
    /// shifted level, and F1 is 67 -- the shape of a US layout.
    fn us_layout() -> (u8, usize, Vec<u32>) {
        let mut keysyms = vec![0u32; 68 * 2];
        keysyms[38 * 2] = 0x61; // XK_a
        keysyms[38 * 2 + 1] = 0x41; // XK_A
        keysyms[10 * 2] = 0x31; // XK_1
        keysyms[11 * 2] = 0x32; // XK_2
        keysyms[21 * 2] = 0x3d; // XK_equal
        keysyms[21 * 2 + 1] = 0x2b; // XK_plus
        keysyms[67 * 2] = 0xffbe; // XK_F1
        (0, 2, keysyms)
    }

    #[test]
    fn a_letter_finds_its_unshifted_keycode() {
        let (min, per, keyboard) = us_layout();
        // Qt reports Key_A (0x41); the unshifted level holds XK_a.
        let matches = keycode_matches(0x41, min, per, &keyboard);
        assert_eq!(
            matches,
            vec![KeycodeMatch {
                keycode: 38,
                implied_shift: false
            }]
        );
    }

    #[test]
    fn a_shifted_symbol_asks_for_shift() {
        let (min, per, keyboard) = us_layout();
        let matches = keycode_matches(0x2b, min, per, &keyboard);
        assert_eq!(
            matches,
            vec![KeycodeMatch {
                keycode: 21,
                implied_shift: true
            }]
        );
    }

    #[test]
    fn an_unshifted_match_wins_over_a_shifted_one() {
        // '1' appears on its own level and (on many layouts) as another key's
        // shifted level; only the reachable-without-Shift key is grabbed.
        let mut keysyms = vec![0u32; 4 * 2];
        keysyms[2 * 2] = 0x31;
        keysyms[3 * 2 + 1] = 0x31;
        let matches = keycode_matches(0x31, 0, 2, &keysyms);
        assert_eq!(matches.len(), 1);
        assert_eq!(matches[0].keycode, 2);
        assert!(!matches[0].implied_shift);
    }

    #[test]
    fn an_absent_keysym_yields_no_match() {
        let (min, per, keyboard) = us_layout();
        assert!(keycode_matches(0x1008_ff14, min, per, &keyboard).is_empty());
    }

    #[test]
    fn a_zero_width_keyboard_does_not_panic() {
        assert!(keycode_matches(0x41, 0, 0, &[0x41, 0x41]).is_empty());
    }

    #[test]
    fn grab_masks_cover_every_lock_combination() {
        let masks = grab_masks(0x04, &[0x02, 0x10]); // Control, CapsLock, NumLock
        assert_eq!(masks.len(), 4);
        assert!(masks.contains(&0x04));
        assert!(masks.contains(&0x06));
        assert!(masks.contains(&0x14));
        assert!(masks.contains(&0x16));
    }

    #[test]
    fn grab_masks_deduplicate_a_lock_already_in_the_binding() {
        // Shift is both the binding's modifier and (in this contrived case) a
        // lock; the grab must not be issued twice for the same mask.
        let masks = grab_masks(0x01, &[0x01]);
        assert_eq!(masks, vec![0x01]);
    }

    #[test]
    fn grab_masks_without_locks_is_just_the_binding() {
        assert_eq!(grab_masks(0x0c, &[]), vec![0x0c]);
        assert_eq!(grab_masks(0x0c, &[0]), vec![0x0c]);
    }

    #[test]
    fn alt_and_super_masks_come_from_the_keyboard() {
        // Mod1 holds Alt_L (keycode 64), Mod4 holds Super_L (133).
        let mut keyboard = vec![0u32; 140 * 2];
        keyboard[64 * 2] = 0xffe9; // XK_Alt_L
        keyboard[133 * 2] = 0xffeb; // XK_Super_L
        let mut modifier_map = vec![Vec::new(); 8];
        modifier_map[3] = vec![64]; // Mod1
        modifier_map[6] = vec![133]; // Mod4
        assert_eq!(
            modifier_mask_for(&modifier_map, 0, 2, &keyboard, &[0xffe9]),
            Some(0x08)
        );
        assert_eq!(
            modifier_mask_for(&modifier_map, 0, 2, &keyboard, &[0xffeb]),
            Some(0x40)
        );
        // A layout with no Super key reports "not present" rather than a guess.
        assert_eq!(
            modifier_mask_for(&modifier_map, 0, 2, &keyboard, &[0x1008_ff14]),
            None
        );
    }

    #[test]
    fn modifier_mask_respects_a_shifted_min_keycode() {
        // Servers usually start at keycode 8; an off-by-min_offset lookup would
        // silently pick the wrong keycode and report a mask for the wrong key.
        let min = 8u8;
        let mut keyboard = vec![0u32; 200 * 2];
        keyboard[usize::from(64 - min) * 2] = 0xffe9;
        let mut modifier_map = vec![Vec::new(); 8];
        modifier_map[3] = vec![64];
        assert_eq!(
            modifier_mask_for(&modifier_map, min, 2, &keyboard, &[0xffe9]),
            Some(0x08)
        );
    }
}
