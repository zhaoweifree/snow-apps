//! Global mouse-drag bindings, translated from Qt terms into X11 grabs.
//!
//! A "global mouse gesture" starts a screenshot when the user holds a modifier
//! chord and drags with a mouse button, anywhere on the desktop. On X11 the only
//! way to see that press *before* the application under the pointer acts on it
//! is a passive `XGrabButton`: the server then delivers the press to this client
//! and to nobody else, which is exactly the "swallow it" half of the feature.
//!
//! That also decides where the matching happens. The server compares the
//! modifier state itself, so a press either matches a grab -- and therefore a
//! configured binding -- or never reaches this process. Nothing here has to
//! decide "is this press ours" at runtime; it only has to decide which grabs to
//! hold.
//!
//! Everything in this module is pure, so the rules are unit-tested on any host.

use crate::hotkey::{KNOWN_MODIFIERS, grab_masks};

/// `Qt::MouseButton` values, as they cross the C ABI.
///
/// Spelled out rather than taken from Qt: this crate has no Qt dependency, and
/// these four numbers are Qt's public enum, not an internal detail.
pub const QT_LEFT_BUTTON: i32 = 1;
pub const QT_RIGHT_BUTTON: i32 = 2;
pub const QT_MIDDLE_BUTTON: i32 = 4;
pub const QT_BACK_BUTTON: i32 = 8;
pub const QT_FORWARD_BUTTON: i32 = 16;

/// One configured gesture: an action, and the chord that starts it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Binding {
    /// The caller's action id, handed back with every event. Not interpreted
    /// here.
    pub action: i32,
    /// A `Qt::MouseButton` value.
    pub qt_button: i32,
    /// Modifier bits, the same encoding [`crate::hotkey`] defines.
    pub modifiers: u32,
}

/// A passive button grab the server can hold.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Grab {
    /// X11 core pointer button number.
    pub button: u8,
    /// Exact modifier state, lock bits included.
    pub mask: u16,
    pub action: i32,
}

/// The X11 core pointer button for a `Qt::MouseButton`.
///
/// The two enums disagree on both ordering and numbering: Qt numbers its buttons
/// as bit flags (1, 2, 4, 8, 16), X11 numbers them in the order the protocol
/// assigned them, which puts the middle button at 2 and the right button at 3,
/// with the wheel on 4..7 and the two side buttons on 8 and 9. Translating by
/// position instead of by name would silently turn a right drag into a middle
/// one.
pub fn x_button(qt_button: i32) -> Option<u8> {
    match qt_button {
        QT_LEFT_BUTTON => Some(1),
        QT_MIDDLE_BUTTON => Some(2),
        QT_RIGHT_BUTTON => Some(3),
        QT_BACK_BUTTON => Some(8),
        QT_FORWARD_BUTTON => Some(9),
        _ => None,
    }
}

/// Expands bindings into the grabs to hold.
///
/// `mask_for` resolves modifier bits to X11 masks; it is a parameter because
/// which bit Alt and Super occupy is a property of the running keyboard, and
/// keeping it out of here is what makes this testable without a server.
///
/// A combination claimed by two different bindings is dropped rather than
/// awarded to the first: the shared gesture logic treats an ambiguous press as
/// no match at all, so grabbing it for one of the two would make the same
/// configuration behave differently on X11 than on the platforms that decide
/// after the event.
pub fn grabs(bindings: &[Binding], mask_for: impl Fn(u32) -> u16, lock_masks: &[u16]) -> Vec<Grab> {
    let mut candidates = Vec::new();
    for binding in bindings {
        // An unknown bit is a caller bug, and guessing which modifier was meant
        // would register a chord one modifier short -- which then fires on
        // presses the user did not ask for.
        if binding.modifiers & !KNOWN_MODIFIERS != 0 {
            continue;
        }
        let Some(button) = x_button(binding.qt_button) else {
            continue;
        };
        for mask in grab_masks(mask_for(binding.modifiers), lock_masks) {
            candidates.push(Grab {
                button,
                mask,
                action: binding.action,
            });
        }
    }

    let contested = |grab: &Grab| {
        candidates
            .iter()
            .filter(|other| other.button == grab.button && other.mask == grab.mask)
            .count()
            > 1
    };
    let mut unique: Vec<Grab> = candidates
        .iter()
        .copied()
        .filter(|grab| !contested(grab))
        .collect();
    unique.sort_by_key(|grab| (grab.button, grab.mask, grab.action));
    unique.dedup();
    unique
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::hotkey::{MODIFIER_ALT, MODIFIER_CONTROL, MODIFIER_META, MODIFIER_SHIFT};

    /// A stand-in for the keyboard tables: Control is 0x04, everything else gets
    /// a bit of its own, which is enough to tell the cases apart.
    fn fake_mask(bits: u32) -> u16 {
        let mut mask = 0;
        if bits & MODIFIER_CONTROL != 0 {
            mask |= 0x0004;
        }
        if bits & MODIFIER_SHIFT != 0 {
            mask |= 0x0001;
        }
        if bits & MODIFIER_ALT != 0 {
            mask |= 0x0008;
        }
        if bits & MODIFIER_META != 0 {
            mask |= 0x0040;
        }
        mask
    }

    const NO_LOCKS: &[u16] = &[];

    #[test]
    fn buttons_map_by_name_not_by_position() {
        assert_eq!(x_button(QT_LEFT_BUTTON), Some(1));
        assert_eq!(x_button(QT_MIDDLE_BUTTON), Some(2));
        assert_eq!(x_button(QT_RIGHT_BUTTON), Some(3));
        assert_eq!(x_button(QT_BACK_BUTTON), Some(8));
        assert_eq!(x_button(QT_FORWARD_BUTTON), Some(9));
    }

    #[test]
    fn unknown_buttons_are_not_grabbed() {
        assert_eq!(x_button(0), None);
        assert_eq!(x_button(32), None);
        assert_eq!(x_button(-1), None);
    }

    #[test]
    fn a_binding_becomes_one_grab_per_lock_variant() {
        let bindings = [Binding {
            action: 3,
            qt_button: QT_LEFT_BUTTON,
            modifiers: MODIFIER_CONTROL,
        }];
        let grabs = grabs(&bindings, fake_mask, &[0x0002, 0x0010]);
        assert_eq!(
            grabs,
            vec![
                Grab {
                    button: 1,
                    mask: 0x0004,
                    action: 3
                },
                Grab {
                    button: 1,
                    mask: 0x0006,
                    action: 3
                },
                Grab {
                    button: 1,
                    mask: 0x0014,
                    action: 3
                },
                Grab {
                    button: 1,
                    mask: 0x0016,
                    action: 3
                },
            ]
        );
    }

    #[test]
    fn lock_bits_are_the_only_variants() {
        let bindings = [Binding {
            action: 0,
            qt_button: QT_RIGHT_BUTTON,
            modifiers: 0,
        }];
        // No modifier at all still needs the lock variants: a user with NumLock
        // on presses the same chord with an extra state bit.
        let grabs = grabs(&bindings, fake_mask, &[0x0002]);
        assert_eq!(grabs.len(), 2);
        assert!(grabs.iter().any(|grab| grab.mask == 0));
        assert!(grabs.iter().any(|grab| grab.mask == 0x0002));
    }

    #[test]
    fn every_modifier_survives_the_round_trip() {
        let bindings = [Binding {
            action: 1,
            qt_button: QT_LEFT_BUTTON,
            modifiers: MODIFIER_CONTROL | MODIFIER_ALT | MODIFIER_SHIFT | MODIFIER_META,
        }];
        let grabs = grabs(&bindings, fake_mask, NO_LOCKS);
        assert_eq!(grabs.len(), 1);
        assert_eq!(grabs[0].mask, 0x0004 | 0x0008 | 0x0001 | 0x0040);
    }

    #[test]
    fn the_same_button_with_different_chords_is_two_grabs() {
        let bindings = [
            Binding {
                action: 1,
                qt_button: QT_LEFT_BUTTON,
                modifiers: MODIFIER_CONTROL,
            },
            Binding {
                action: 2,
                qt_button: QT_LEFT_BUTTON,
                modifiers: MODIFIER_SHIFT,
            },
        ];
        let grabs = grabs(&bindings, fake_mask, NO_LOCKS);
        assert_eq!(grabs.len(), 2);
        assert_eq!(grabs[0].mask, 0x0001);
        assert_eq!(grabs[0].action, 2);
        assert_eq!(grabs[1].mask, 0x0004);
        assert_eq!(grabs[1].action, 1);
    }

    #[test]
    fn a_chord_claimed_twice_is_dropped_entirely() {
        let bindings = [
            Binding {
                action: 1,
                qt_button: QT_LEFT_BUTTON,
                modifiers: MODIFIER_CONTROL,
            },
            Binding {
                action: 2,
                qt_button: QT_LEFT_BUTTON,
                modifiers: MODIFIER_CONTROL,
            },
        ];
        assert!(grabs(&bindings, fake_mask, &[0x0002]).is_empty());
    }

    #[test]
    fn an_unknown_modifier_bit_skips_the_binding() {
        let bindings = [Binding {
            action: 1,
            qt_button: QT_LEFT_BUTTON,
            modifiers: MODIFIER_CONTROL | (1 << 7),
        }];
        assert!(grabs(&bindings, fake_mask, NO_LOCKS).is_empty());
    }

    #[test]
    fn an_unknown_button_is_skipped_without_taking_the_others_with_it() {
        let bindings = [
            Binding {
                action: 1,
                qt_button: 64,
                modifiers: 0,
            },
            Binding {
                action: 2,
                qt_button: QT_MIDDLE_BUTTON,
                modifiers: 0,
            },
        ];
        let grabs = grabs(&bindings, fake_mask, NO_LOCKS);
        assert_eq!(grabs.len(), 1);
        assert_eq!(grabs[0].button, 2);
        assert_eq!(grabs[0].action, 2);
    }

    #[test]
    fn an_empty_configuration_grabs_nothing() {
        assert!(grabs(&[], fake_mask, &[0x0002, 0x0010]).is_empty());
    }

    #[test]
    fn grabs_are_ordered_by_button_then_mask() {
        let bindings = [
            Binding {
                action: 5,
                qt_button: QT_FORWARD_BUTTON,
                modifiers: 0,
            },
            Binding {
                action: 4,
                qt_button: QT_LEFT_BUTTON,
                modifiers: MODIFIER_SHIFT,
            },
            Binding {
                action: 6,
                qt_button: QT_LEFT_BUTTON,
                modifiers: 0,
            },
        ];
        let grabs = grabs(&bindings, fake_mask, &[0x0002]);
        let order: Vec<(u8, u16)> = grabs.iter().map(|grab| (grab.button, grab.mask)).collect();
        let mut sorted = order.clone();
        sorted.sort_unstable();
        assert_eq!(order, sorted);
        // Left button first, and its unmodified chord before its Shift chord.
        assert_eq!(order.first(), Some(&(1, 0)));
        assert_eq!(order.last(), Some(&(9, 0x0002)));
    }

    #[test]
    fn the_action_id_travels_with_every_variant() {
        let bindings = [Binding {
            action: 42,
            qt_button: QT_BACK_BUTTON,
            modifiers: MODIFIER_ALT,
        }];
        for grab in grabs(&bindings, fake_mask, &[0x0002, 0x0010]) {
            assert_eq!(grab.action, 42);
            assert_eq!(grab.button, 8);
        }
    }
}
