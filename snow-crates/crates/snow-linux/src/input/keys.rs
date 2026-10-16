//! Keysym / keycode translation for the X11 input observer.
//!
//! Everything here is pure: it takes the numbers an X server reports and turns
//! them into the *Windows-style virtual-key identities* the recording overlay is
//! written against. Keeping it out of `#[cfg(target_os = "linux")]` is what makes
//! the translation unit-testable on any host (see the crate docs).
//!
//! # Why the overlay needs Windows virtual-key codes
//!
//! [`snow_recording_effects`] renders keycaps through
//! `KeyObservation::pressed` and `keyboard_overlay::modifier`, both of which
//! index a 256-entry table with hard-coded identities: `0xa2`/`0xa3` for the two
//! Control keys, `0xa4`/`0xa5` for Alt/AltGr, `0xa0`/`0xa1` for Shift, `0x5b`/
//! `0x5c` for Super, `0x70..=0x87` for F1–F24 and `0x60..=0x69` for the keypad.
//! A Linux backend must therefore speak that space rather than raw X11
//! keycodes, otherwise every label and every modifier chip comes out wrong.
//!
//! # Keysyms describe characters, not keys
//!
//! X11 reports the *symbol produced* by a keycode at a given shift level, so a
//! US layout reports `0x31` (`1`) unshifted and `0x21` (`!`) shifted for the same
//! key. Windows virtual-key codes describe the *key itself* (`0x31` for both).
//! [`virtual_key`] therefore normalises both members of a shift pair onto the
//! same identity, which is why it is a table over characters and not a copy of
//! the keysym numbering.

/// X11 reserves keycodes `0..=7`; the server's mapping always starts at 8.
pub const FIRST_KEYCODE: u8 = 8;

/// Keycodes addressable by the server: `8..=255`.
pub const KEYCODE_COUNT: usize = 256 - FIRST_KEYCODE as usize;

/// Shift level used when the mapping has a single column.
const LEVEL_BASE: u8 = 0;

/// A server keyboard mapping (`GetKeyboardMapping`).
///
/// Holds `keysyms_per_keycode` symbols for every keycode from
/// [`KeyMap::first_keycode`] onwards, in row-major order.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct KeyMap {
    first_keycode: u8,
    keysyms_per_keycode: u8,
    keysyms: Vec<u32>,
}

impl KeyMap {
    /// Validate a raw `GetKeyboardMapping` reply.
    ///
    /// Returns `None` for a mapping whose length does not match its own
    /// `keysyms_per_keycode`, or whose column count is zero: both mean the
    /// translation tables would be read out of bounds or produce nothing, and
    /// neither can be repaired by retrying.
    pub fn new(first_keycode: u8, keysyms_per_keycode: u8, keysyms: &[u32]) -> Option<Self> {
        if keysyms_per_keycode == 0 || first_keycode < FIRST_KEYCODE {
            return None;
        }
        let columns = usize::from(keysyms_per_keycode);
        if keysyms.len() % columns != 0 {
            return None;
        }
        let rows = keysyms.len() / columns;
        if rows > usize::from(256u16 - u16::from(first_keycode)) {
            return None;
        }
        Some(Self {
            first_keycode,
            keysyms_per_keycode,
            keysyms: keysyms.to_vec(),
        })
    }

    pub fn first_keycode(&self) -> u8 {
        self.first_keycode
    }

    pub fn keysyms_per_keycode(&self) -> u8 {
        self.keysyms_per_keycode
    }

    /// Symbol at an explicit shift level, without any `NoSymbol` fallback.
    pub fn keysym_at(&self, keycode: u8, level: u8) -> Option<u32> {
        if keycode < self.first_keycode {
            return None;
        }
        let row = usize::from(keycode - self.first_keycode);
        let column = usize::from(level);
        if column >= usize::from(self.keysyms_per_keycode) {
            return None;
        }
        let index = row * usize::from(self.keysyms_per_keycode) + column;
        self.keysyms.get(index).copied()
    }

    /// Symbol a keycode produces with the given modifier state.
    ///
    /// Falls back to the base level when the selected column is `NoSymbol`,
    /// which is the common shape of a mapping that only fills the levels it
    /// needs (for example a keypad key with no AltGr column).
    pub fn keysym(&self, keycode: u8, shift: bool, alt_gr: bool) -> Option<u32> {
        let level = self.level(shift, alt_gr);
        let selected = self.keysym_at(keycode, level).filter(|sym| *sym != NO_SYMBOL);
        selected.or_else(|| {
            self.keysym_at(keycode, LEVEL_BASE)
                .filter(|sym| *sym != NO_SYMBOL)
        })
    }

    /// Shift level index for a modifier state.
    ///
    /// X11 has no fixed column count, so the index has to be derived from it:
    /// 1 column is unshifted only, 2 adds Shift, 3 adds AltGr and 4+ also allows
    /// AltGr with Shift. A level the mapping does not have must not silently
    /// borrow another one, so AltGr with two columns stays on the base level.
    pub fn level(&self, shift: bool, alt_gr: bool) -> u8 {
        match self.keysyms_per_keycode {
            0 | 1 => 0,
            2 => u8::from(shift),
            3 => {
                if alt_gr {
                    2
                } else {
                    u8::from(shift)
                }
            }
            _ => u8::from(shift) + 2 * u8::from(alt_gr),
        }
    }
}

/// `NoSymbol`: the column exists but produces nothing.
pub const NO_SYMBOL: u32 = 0;

/// Key identity for a symbol, in the Windows virtual-key space.
///
/// Returns `None` for symbols this table does not name (Cyrillic and other
/// non-Latin symbols, exotic XF86 keys). Callers report the raw keycode instead
/// of inventing an identity, so the overlay shows the real label rather than a
/// plausible-looking wrong one.
pub fn virtual_key(keysym: u32) -> Option<u16> {
    // Named keys first: the ASCII range below would otherwise claim `0x20`
    // (space) and nothing else, but a modifier symbol like `0xffe1` must never
    // fall through to the character table.
    if let Some(key) = named_virtual_key(keysym) {
        return Some(key);
    }
    let ch = character(keysym)?;
    character_virtual_key(ch)
}

/// Virtual-key identity for an explicit character.
///
/// Both members of a shift pair normalise onto the same key, so `!` reports the
/// identity of `1` and `?` the identity of `/`. Latin letters map onto
/// `VK_A..VK_Z` by character, which is exactly Windows semantics: the virtual
/// key names the key that *produces* that character under the active layout.
fn character_virtual_key(ch: char) -> Option<u16> {
    if ch.is_ascii_uppercase() || ch.is_ascii_lowercase() {
        let upper = ch.to_ascii_uppercase();
        return u32::from(upper)
            .checked_sub(u32::from('A'))
            .and_then(|offset| u16::try_from(0x41 + offset).ok());
    }
    if ch.is_ascii_digit() {
        return u16::try_from(0x30 + u32::from(ch) - u32::from('0')).ok();
    }
    Some(match ch {
        ' ' => 0x20,
        // Unshifted and shifted members of the same physical key share an
        // identity, mirroring `VK_OEM_*` on Windows.
        '`' | '~' => 0xc0,
        '-' | '_' => 0xbd,
        '=' | '+' => 0xbb,
        '[' | '{' => 0xdb,
        ']' | '}' => 0xdd,
        '\\' | '|' => 0xdc,
        ';' | ':' => 0xba,
        '\'' | '"' => 0xde,
        ',' | '<' => 0xbc,
        '.' | '>' => 0xbe,
        '/' | '?' => 0xbf,
        // Shifted digits: `!` is the `1` key, `@` the `2` key, and so on.
        ')' => 0x30,
        '!' => 0x31,
        '@' => 0x32,
        '#' => 0x33,
        '$' => 0x34,
        '%' => 0x35,
        '^' => 0x36,
        '&' => 0x37,
        '*' => 0x38,
        '(' => 0x39,
        _ => return None,
    })
}

/// The X11 symbols this table names explicitly.
fn named_virtual_key(keysym: u32) -> Option<u16> {
    Some(match keysym {
        0xff08 => 0x08, // BackSpace
        0xff09 => 0x09, // Tab
        0xff0d => 0x0d, // Return
        0xff13 => 0x13, // Pause
        0xff14 => 0x14, // Scroll_Lock
        0xff1b => 0x1b, // Escape
        0xff50 => 0x24, // Home
        0xff51 => 0x25, // Left
        0xff52 => 0x26, // Up
        0xff53 => 0x27, // Right
        0xff54 => 0x28, // Down
        0xff55 => 0x21, // Prior (Page Up)
        0xff56 => 0x22, // Next (Page Down)
        0xff57 => 0x23, // End
        0xff58 => 0x0c, // Begin, the key `VK_CLEAR` names
        0xff61 => 0x2c, // Print
        0xff63 => 0x2d, // Insert
        0xff67 => 0x5d, // Menu
        0xff69 => 0x03, // Cancel
        0xff6a => 0x2f, // Help
        0xff6b => 0x13, // Break
        0xff7e => 0xa5, // Mode_switch (AltGr on most layouts)
        0xff7f => 0x90, // Num_Lock
        0xff8d => 0x0d, // KP_Enter
        0xff95 => 0x24, // KP_Home
        0xff96 => 0x25, // KP_Left
        0xff97 => 0x26, // KP_Up
        0xff98 => 0x27, // KP_Right
        0xff99 => 0x28, // KP_Down
        0xff9a => 0x21, // KP_Prior
        0xff9b => 0x22, // KP_Next
        0xff9c => 0x23, // KP_End
        0xff9d => 0x0c, // KP_Begin
        0xff9e => 0x2d, // KP_Insert
        0xff9f => 0x2e, // KP_Delete
        0xffaa => 0x6a, // KP_Multiply
        0xffab => 0x6b, // KP_Add
        0xffac => 0x6c, // KP_Separator
        0xffad => 0x6d, // KP_Subtract
        0xffae => 0x6e, // KP_Decimal
        0xffaf => 0x6f, // KP_Divide
        // Keypad digits stay in the `VK_NUMPAD*` block so the overlay's
        // `Num N` labelling keeps working.
        0xffb0..=0xffb9 => 0x60 + u16::try_from(keysym - 0xffb0).ok()?,
        // Function keys land in `VK_F1..VK_F24`, the range the overlay
        // formats as `Fn` without asking the platform layer for a label.
        0xffbe..=0xffd5 => 0x70 + u16::try_from(keysym - 0xffbe).ok()?,
        0xffe1 => 0xa0, // Shift_L
        0xffe2 => 0xa1, // Shift_R
        0xffe3 => 0xa2, // Control_L
        0xffe4 => 0xa3, // Control_R
        0xffe5 => 0x14, // Caps_Lock
        0xffe6 => 0x14, // Shift_Lock is the lock key, not a Shift
        0xffe7 => 0x5b, // Meta_L
        0xffe8 => 0x5c, // Meta_R
        0xffe9 => 0xa4, // Alt_L
        0xffea => 0xa5, // Alt_R
        0xffeb => 0x5b, // Super_L
        0xffec => 0x5c, // Super_R
        0xffed => 0x5b, // Hyper_L
        0xffee => 0x5c, // Hyper_R
        // `ISO_Level3_Shift` is the physical AltGr key on most layouts; it has
        // to share the right-Alt identity so the overlay renders AltGr.
        0xfe03 => 0xa5,
        0xfe11 => 0xa5, // ISO_Level5_Shift
        // Media and browser keys have real virtual-key codes, which keeps them
        // distinguishable in the overlay instead of collapsing onto a fallback.
        0x1008ff11 => 0xae, // XF86AudioLowerVolume
        0x1008ff12 => 0xad, // XF86AudioMute
        0x1008ff13 => 0xaf, // XF86AudioRaiseVolume
        0x1008ff14 => 0xb3, // XF86AudioPlay
        0x1008ff15 => 0xb2, // XF86AudioStop
        0x1008ff16 => 0xb1, // XF86AudioPrev
        0x1008ff17 => 0xb0, // XF86AudioNext
        0x1008ff19 => 0xb4, // XF86Mail
        0x1008ff26 => 0xa6, // XF86Back
        0x1008ff27 => 0xa7, // XF86Forward
        0x1008ff2f => 0x5f, // XF86Sleep
        _ => return None,
    })
}

/// The character a symbol produces, if it is a character at all.
///
/// Handles the Latin-1 range and the Unicode keysym block (`0x01000000` plus a
/// code point) that modern layouts use for anything outside it.
pub fn character(keysym: u32) -> Option<char> {
    if let Some(offset) = keysym.checked_sub(0x0100_0000) {
        // The block is reserved from 0x01000100; below it the value is not a
        // Unicode keysym.
        if offset < 0x100 {
            return None;
        }
        return char::from_u32(offset);
    }
    if (0x20..0x100).contains(&keysym) {
        return char::from_u32(keysym);
    }
    None
}

/// Human-readable label for a symbol.
///
/// Named keys get a printable name; characters report themselves so the overlay
/// shows the symbol the user actually typed, including the shifted one
/// (`!`, `Д`). Returns `None` when the symbol has neither, and the caller falls
/// back to the virtual-key number.
pub fn label(keysym: u32) -> Option<String> {
    if let Some(name) = named_label(keysym) {
        return Some(name.to_string());
    }
    let ch = character(keysym)?;
    // A control character is not something to paint on a keycap.
    if ch.is_control() {
        return None;
    }
    Some(ch.to_string())
}

/// Names for the symbols that are not characters.
fn named_label(keysym: u32) -> Option<&'static str> {
    Some(match keysym {
        0x20 => "Space",
        0xff08 => "Backspace",
        0xff09 => "Tab",
        0xff0d => "Return",
        0xff13 => "Pause",
        0xff14 => "Scroll Lock",
        0xff1b => "Esc",
        0xff50 => "Home",
        0xff51 => "Left",
        0xff52 => "Up",
        0xff53 => "Right",
        0xff54 => "Down",
        0xff55 => "Page Up",
        0xff56 => "Page Down",
        0xff57 | 0xff58 => "End",
        0xff61 => "Print Screen",
        0xff63 => "Insert",
        0xff65 => "Undo",
        0xff66 => "Redo",
        0xff67 => "Menu",
        0xff6a => "Help",
        0xff6b => "Break",
        0xff7e | 0xfe03 | 0xfe11 => "AltGr",
        0xff7f => "Num Lock",
        0xff8d => "Enter",
        0xff95 => "Home",
        0xff96 => "Left",
        0xff97 => "Up",
        0xff98 => "Right",
        0xff99 => "Down",
        0xff9a => "Page Up",
        0xff9b => "Page Down",
        0xff9c => "End",
        0xff9e => "Insert",
        0xff9f => "Delete",
        0xffaa => "Num *",
        0xffab => "Num +",
        0xffac => "Num ,",
        0xffad => "Num -",
        0xffae => "Num .",
        0xffaf => "Num /",
        0xffe1 | 0xffe2 | 0xffe6 => "Shift",
        0xffe3 | 0xffe4 => "Ctrl",
        0xffe5 => "Caps Lock",
        0xffe7 | 0xffe9 | 0xffea => "Alt",
        0xffe8 => "Alt",
        0xffeb..=0xffee => "Super",
        0x1008ff02 => "Brightness Up",
        0x1008ff03 => "Brightness Down",
        0x1008ff11 => "Volume Down",
        0x1008ff12 => "Mute",
        0x1008ff13 => "Volume Up",
        0x1008ff14 | 0x1008ff32 => "Play",
        0x1008ff15 => "Stop",
        0x1008ff16 => "Previous",
        0x1008ff17 => "Next",
        0x1008ff18 => "Home Page",
        0x1008ff19 => "Mail",
        0x1008ff1b => "Search",
        0x1008ff26 => "Back",
        0x1008ff27 => "Forward",
        0x1008ff2b => "Wake Up",
        0x1008ff2f => "Sleep",
        _ => return None,
    })
}

/// Name of a *generic* virtual key the overlay asks about by number.
///
/// `KeyObservation::event` labels modifier chips with the generic codes
/// `0x10`/`0x11`/`0x12`/`0x5b` rather than the side-specific ones, so the
/// platform layer has to name them without a keysym in hand.
pub fn virtual_key_label(key: u16) -> Option<&'static str> {
    Some(match key {
        0x10 | 0xa0 | 0xa1 => "Shift",
        0x11 | 0xa2 | 0xa3 => "Ctrl",
        0x12 | 0xa4 => "Alt",
        0xa5 => "AltGr",
        0x5b | 0x5c => "Super",
        _ => return None,
    })
}

/// Apply one key transition to a virtual-key-indexed pressed table.
///
/// A symbol the table cannot name is dropped: it has no identity to hold down,
/// and inventing one would leave a permanently stuck modifier behind.
pub fn apply(pressed: &mut [u8; 256], keysym: u32, down: bool) {
    if let Some(key) = virtual_key(keysym) {
        pressed[usize::from(key)] = if down { 0x80 } else { 0 };
    }
}

/// Rebuild the pressed table from a `QueryKeymap` bitmap.
///
/// Used to resynchronise after an observed gap: the bitmap is the server's own
/// view of every keycode, so no key pressed before the observer started, or
/// during a dropped batch, can be missed.
pub fn pressed_from_keymap(bits: &[u8; 32], map: &KeyMap, pressed: &mut [u8; 256]) {
    pressed.fill(0);
    for (index, byte) in bits.iter().enumerate() {
        if *byte == 0 {
            continue;
        }
        for bit in 0..8u8 {
            if byte & (1 << bit) == 0 {
                continue;
            }
            let keycode = u8::try_from(index * 8 + usize::from(bit)).unwrap_or(u8::MAX);
            // The shift level does not matter for identity: both members of a
            // pair map onto the same virtual key.
            if let Some(keysym) = map.keysym(keycode, false, false) {
                apply(pressed, keysym, true);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Four-column US mapping for one keycode, the shape the server reports.
    fn us_map(first_keycode: u8, rows: &[&[u32]]) -> KeyMap {
        let per = u8::try_from(rows[0].len()).unwrap();
        let keysyms: Vec<u32> = rows.iter().flat_map(|row| row.iter().copied()).collect();
        KeyMap::new(first_keycode, per, &keysyms).expect("well-formed mapping")
    }

    #[test]
    fn letters_and_digits_keep_their_ascii_identity() {
        assert_eq!(virtual_key(u32::from('a')), Some(0x41));
        assert_eq!(virtual_key(u32::from('z')), Some(0x5a));
        assert_eq!(virtual_key(u32::from('7')), Some(0x37));
    }

    #[test]
    fn shift_pairs_normalise_onto_one_key_identity() {
        // The whole point of mapping characters instead of keysyms: `1` and `!`
        // are the same physical key, so they must share one overlay identity.
        assert_eq!(virtual_key(u32::from('1')), virtual_key(u32::from('!')));
        assert_eq!(virtual_key(u32::from('/')), virtual_key(u32::from('?')));
        assert_eq!(virtual_key(u32::from(',')), virtual_key(u32::from('<')));
        assert_eq!(virtual_key(u32::from('\'')), virtual_key(u32::from('"')));
        assert_eq!(virtual_key(u32::from('=')), Some(0xbb));
        assert_eq!(virtual_key(u32::from('\\')), Some(0xdc));
    }

    #[test]
    fn modifiers_map_to_side_specific_codes() {
        assert_eq!(virtual_key(0xffe3), Some(0xa2));
        assert_eq!(virtual_key(0xffe4), Some(0xa3));
        assert_eq!(virtual_key(0xffe9), Some(0xa4));
        assert_eq!(virtual_key(0xffea), Some(0xa5));
        assert_eq!(virtual_key(0xffe1), Some(0xa0));
        assert_eq!(virtual_key(0xffe2), Some(0xa1));
        assert_eq!(virtual_key(0xffeb), Some(0x5b));
        assert_eq!(virtual_key(0xffec), Some(0x5c));
    }

    #[test]
    fn altgr_symbols_share_the_right_alt_identity() {
        // `ISO_Level3_Shift` is AltGr on the layouts that have it, and the
        // overlay renders `0xa5` as AltGr, so both symbols must land there.
        assert_eq!(virtual_key(0xfe03), Some(0xa5));
        assert_eq!(virtual_key(0xff7e), Some(0xa5));
        assert_eq!(label(0xfe03).as_deref(), Some("AltGr"));
    }

    #[test]
    fn function_keys_land_in_the_overlay_range() {
        // `KeyObservation::event` formats `0x70..=0x87` as `Fn` by itself, so a
        // wrong base would show up as `VK 6F` instead of `F1`.
        assert_eq!(virtual_key(0xffbe), Some(0x70));
        assert_eq!(virtual_key(0xffc9), Some(0x7b));
        assert_eq!(virtual_key(0xffd5), Some(0x87));
    }

    #[test]
    fn keypad_keys_keep_their_own_block() {
        assert_eq!(virtual_key(0xffb0), Some(0x60));
        assert_eq!(virtual_key(0xffb9), Some(0x69));
        assert_eq!(virtual_key(0xffaf), Some(0x6f));
        assert_eq!(virtual_key(0xff8d), Some(0x0d));
        assert_eq!(virtual_key(0xff95), Some(0x24));
        assert_eq!(virtual_key(0xff9f), Some(0x2e));
    }

    #[test]
    fn keymap_reads_shift_and_altgr_levels() {
        let map = us_map(8, &[&[u32::from('a'), u32::from('A'), 0xe6, 0xc6]]);
        assert_eq!(map.keysym(8, false, false), Some(u32::from('a')));
        assert_eq!(map.keysym(8, true, false), Some(u32::from('A')));
        assert_eq!(map.keysym(8, false, true), Some(0xe6));
        assert_eq!(map.keysym(8, true, true), Some(0xc6));
    }

    #[test]
    fn keymap_falls_back_to_the_base_level_for_missing_symbols() {
        // A two-column mapping has no AltGr column at all, so AltGr must not
        // borrow the shifted one.
        let two = us_map(8, &[&[u32::from('a'), u32::from('A')]]);
        assert_eq!(two.level(false, true), 0);
        assert_eq!(two.keysym(8, false, true), Some(u32::from('a')));
        assert_eq!(two.keysym(8, true, false), Some(u32::from('A')));

        // Three columns do have AltGr, but not AltGr with Shift.
        let three = us_map(8, &[&[u32::from('a'), u32::from('A'), 0xe6]]);
        assert_eq!(three.level(true, true), 2);
        assert_eq!(three.keysym(8, true, true), Some(0xe6));

        // A column that exists but holds NoSymbol falls back rather than
        // reporting that the key produces nothing.
        let sparse = us_map(8, &[&[u32::from('a'), NO_SYMBOL, NO_SYMBOL, NO_SYMBOL]]);
        assert_eq!(sparse.keysym(8, false, true), Some(u32::from('a')));
    }

    #[test]
    fn keymap_addresses_only_keycodes_the_server_mapped() {
        let map = us_map(8, &[&[u32::from('a'), u32::from('A')], &[0xffe1, 0xffe1]]);
        assert_eq!(map.keysym_at(9, 0), Some(0xffe1));
        // Below `first_keycode` and past the last row are both absent, not
        // silently read from a neighbouring row.
        assert_eq!(map.keysym_at(7, 0), None);
        assert_eq!(map.keysym_at(10, 0), None);
        assert_eq!(map.keysym_at(8, 2), None);
    }

    #[test]
    fn malformed_mappings_are_rejected_outright() {
        // Zero columns can never produce a symbol.
        assert!(KeyMap::new(8, 0, &[]).is_none());
        // A length that is not a whole number of rows cannot be indexed.
        assert!(KeyMap::new(8, 2, &[u32::from('a')]).is_none());
        // Server mappings start at keycode 8.
        assert!(KeyMap::new(4, 2, &[0, 0, 0, 0]).is_none());
        // More rows than the keycode space can address.
        let rows = 250;
        assert!(KeyMap::new(8, 1, &vec![0u32; rows]).is_none());
    }

    #[test]
    fn pressed_bitmap_uses_the_server_keycode_bits() {
        let map = us_map(8, &[&[u32::from('a'), u32::from('A')]]);
        // Bit 0 of byte 1 is keycode 8, which the mapping calls `a`.
        let mut bits = [0u8; 32];
        bits[1] = 0b0000_0001;
        let mut pressed = [0u8; 256];
        pressed_from_keymap(&bits, &map, &mut pressed);
        assert_eq!(pressed[0x41], 0x80);
        assert_eq!(pressed.iter().filter(|slot| **slot != 0).count(), 1);
    }

    #[test]
    fn resynchronising_clears_keys_that_were_released_unseen() {
        let map = us_map(8, &[&[u32::from('a'), u32::from('A')]]);
        let mut pressed = [0u8; 256];
        apply(&mut pressed, u32::from('a'), true);
        apply(&mut pressed, 0xffe3, true);
        assert_eq!(pressed[0x41], 0x80);
        assert_eq!(pressed[0xa2], 0x80);

        let bits = [0u8; 32];
        pressed_from_keymap(&bits, &map, &mut pressed);
        assert_eq!(pressed, [0u8; 256]);
    }

    #[test]
    fn unnamed_symbols_report_no_identity_instead_of_guessing() {
        // Cyrillic `Д`: a real character with a real label, but no Windows
        // virtual key, so the caller falls back to the keycode identity.
        let cyrillic = 0x0100_0000 + u32::from('Д');
        assert_eq!(virtual_key(cyrillic), None);
        assert_eq!(label(cyrillic).as_deref(), Some("Д"));
        // An XF86 key outside the mapped set behaves the same way.
        assert_eq!(virtual_key(0x1008ff99), None);
        // Brightness has no Windows virtual key. Inventing one would collide
        // with a real key (the volume keys live right next to it in the XF86
        // block), so it is reported by keycode with a readable label instead.
        assert_eq!(virtual_key(0x1008ff02), None);
        assert_eq!(label(0x1008ff02).as_deref(), Some("Brightness Up"));
    }

    #[test]
    fn unicode_keysyms_decode_to_their_code_point() {
        assert_eq!(character(0x0100_0414), char::from_u32(0x0414));
        // Below the reserved offset the block is not a Unicode keysym.
        assert_eq!(character(0x0100_0041), None);
        // Latin-1 symbols decode directly.
        assert_eq!(character(0xe6), char::from_u32(0xe6));
        // Control symbols are not characters to paint.
        assert_eq!(character(0x0d), None);
    }

    #[test]
    fn labels_name_keys_the_overlay_cannot_format_itself() {
        assert_eq!(label(0xff0d).as_deref(), Some("Return"));
        assert_eq!(label(0xff1b).as_deref(), Some("Esc"));
        assert_eq!(label(0x20).as_deref(), Some("Space"));
        // Shifted characters report themselves, which is what makes the
        // keycap show `!` rather than `1`.
        assert_eq!(label(u32::from('!')).as_deref(), Some("!"));
        assert_eq!(label(0x1008ff12).as_deref(), Some("Mute"));
        // A symbol can be labelled without having a Windows identity: the
        // overlay still shows `Undo`, it just carries a keycode-based key.
        assert_eq!(virtual_key(0xff65), None);
        assert_eq!(label(0xff65).as_deref(), Some("Undo"));
        // Neither a name nor a character: the caller falls back to `VK xx`.
        assert_eq!(label(0xffff_0000), None);
    }

    #[test]
    fn generic_modifier_numbers_are_named_for_the_overlay() {
        // `KeyObservation::event` asks about the generic codes, not the sides.
        assert_eq!(virtual_key_label(0x10), Some("Shift"));
        assert_eq!(virtual_key_label(0x11), Some("Ctrl"));
        assert_eq!(virtual_key_label(0x12), Some("Alt"));
        assert_eq!(virtual_key_label(0x5b), Some("Super"));
        assert_eq!(virtual_key_label(0xa5), Some("AltGr"));
        assert_eq!(virtual_key_label(0x41), None);
    }
}
