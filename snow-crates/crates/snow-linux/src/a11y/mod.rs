//! AT-SPI2: the widget tree that the X11 core protocol does not have.
//!
//! X11 knows geometry and pixels. It does not know that a widget holds text, let
//! alone which part of that text is selected -- which is why the Linux
//! selected-text backend can read the PRIMARY selection but cannot report a
//! rectangle to place a translation bubble on. AT-SPI2 is where that lives: a
//! second D-Bus connection (the *accessibility bus*) on which every toolkit
//! publishes its widget tree.
//!
//! Everything that can be decided without a bus lives here: object references
//! and their inheritance rule, the state values, character offsets versus bytes,
//! extents sanity, and the search itself (bounded, cycle-safe, preferring a node
//! that actually carries text). The transport is [`bus`]; keeping the two apart
//! is what lets this half be tested on a machine that has never run a desktop
//! bus.
//!
//! Four rules in here are easy to get wrong and expensive to debug on a real
//! machine, so they are stated here rather than buried in the transport:
//!
//! 1. **Text offsets are characters, not bytes.** They are UCS-4 code points,
//!    so a byte index into the UTF-8 reply is right for ASCII and wrong for
//!    every Chinese character. See [`char_range_to_byte_range`].
//! 2. **An empty bus name means "the same application as the parent".** The
//!    spec allows a child reference to omit it, and a client that takes the
//!    empty string literally addresses no one. See [`inherit_name`].
//! 3. **`FOCUSED` does not mean "this is the text".** Window frames and buttons
//!    carry it too, so the walk prefers a focused node that also implements
//!    `org.a11y.atspi.Text` and keeps the first plain one as a fallback.
//! 4. **A state list is a list of enum values, not a bitmask.** Testing it with
//!    `states & (1 << FOCUSED)` compiles, runs, and is silently wrong.

use std::collections::{HashSet, VecDeque};
use std::ops::Range;

use crate::geometry::Rect;

#[cfg(target_os = "linux")]
pub mod bus;

/// The accessibility registry: owner of the desktop root object.
pub const REGISTRY_SERVICE: &str = "org.a11y.atspi.Registry";
/// The desktop itself -- the parent of every application.
pub const ROOT_PATH: &str = "/org/a11y/atspi/accessible/root";
/// The reference an interface returns for "there is no object here".
pub const NULL_PATH: &str = "/org/a11y/atspi/null";

pub const ACCESSIBLE_INTERFACE: &str = "org.a11y.atspi.Accessible";
pub const TEXT_INTERFACE: &str = "org.a11y.atspi.Text";
pub const COMPONENT_INTERFACE: &str = "org.a11y.atspi.Component";

/// The largest selection this code will read, in bytes.
///
/// A selection is a phrase, not a document; the cap exists so a toolkit that
/// answers `GetSelection` with "everything" cannot make this process allocate
/// without bound. Truncation happens on a character boundary
/// ([`clamp_text`]), so the result is always valid UTF-8.
pub const MAX_TEXT_BYTES: usize = 64 * 1024;

/// The `AtspiStateType` values this code tests for.
///
/// Taken from the declaration order in `atspi/atspi-constants.h`. New states are
/// appended to that enum, so these numbers are stable; the test below pins them
/// against an accidental edit, and the doc comments name them so a reader can
/// re-derive them from the header.
pub mod state {
    /// `ATSPI_STATE_DEFUNCT`: the widget is gone. Its children are stale too.
    pub const DEFUNCT: u32 = 6;
    /// `ATSPI_STATE_FOCUSED`: holds the keyboard focus.
    pub const FOCUSED: u32 = 12;
    /// `ATSPI_STATE_SELECTABLE_TEXT`: has a text cursor (not necessarily a
    /// selection). Useful when a caller wants the caret rather than a range.
    pub const SELECTABLE_TEXT: u32 = 38;
}

/// Whether a state list contains `state`.
///
/// `GetState` returns `au` -- the enum values of the states that hold, in no
/// particular order. Membership, never arithmetic.
pub fn has_state(states: &[u32], state: u32) -> bool {
    states.contains(&state)
}

/// Whether the object is marked defunct.
///
/// The walk does not descend into a defunct subtree: the toolkit has already
/// disowned it, so every child read from it is either stale or an error, and
/// both cost round trips to learn nothing.
pub fn is_defunct(states: &[u32]) -> bool {
    has_state(states, state::DEFUNCT)
}

/// Whether an interface list includes `Text`.
///
/// Exact match on purpose: `starts_with("org.a11y.atspi.Text")` would also
/// accept a hypothetical `org.a11y.atspi.TextInput`, and then the read would
/// call `GetNSelections` on something that does not implement it.
pub fn supports_text(interfaces: &[String]) -> bool {
    interfaces.iter().any(|name| name == TEXT_INTERFACE)
}

/// A `(so)` object reference, normalised into owned strings.
///
/// `name` is a *bus name on the accessibility bus*: a unique name such as
/// `:1.42` for an application, or the registry's well-known name for the
/// desktop root. `path` is that connection's object path.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct ObjectRef {
    pub name: String,
    pub path: String,
}

impl ObjectRef {
    pub fn new(name: impl Into<String>, path: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            path: path.into(),
        }
    }

    /// The desktop root, as addressed by the registry.
    pub fn desktop() -> Self {
        Self::new(REGISTRY_SERVICE, ROOT_PATH)
    }

    /// Whether this reference points at nothing.
    ///
    /// Interface methods return the sentinel path rather than an error when a
    /// child does not exist, and some toolkits return both fields empty.
    pub fn is_null(&self) -> bool {
        self.path.is_empty() || self.path == NULL_PATH
    }

    /// Resolve a child reference's bus name against its parent.
    ///
    /// `GetChildren` may leave the bus name empty to mean "the same connection
    /// as the object you asked". A client that keeps the empty string sends its
    /// next call to the bus daemon itself (which is not even on the
    /// accessibility bus) instead of to the application.
    pub fn child(&self, child: Self) -> Self {
        Self {
            name: if child.name.is_empty() {
                self.name.clone()
            } else {
                child.name
            },
            path: child.path,
        }
    }
}

/// Convert a character range to a byte range within `text`.
///
/// AT-SPI2 offsets count characters -- UCS-4 code points -- while Rust strings
/// are indexed by bytes. For `"中文"` the selection `(1, 2)` is the second
/// character, which is bytes `3..6`; slicing `&text[1..2]` panics on a
/// non-boundary.
///
/// `end` past the end of the text is clamped rather than refused: toolkits
/// compute it before the read and the text may have changed in between, and a
/// clamped read still returns the useful prefix. A `start` past the end is
/// refused: it means the two sides disagree about what the text is, and
/// returning an empty string would look like an empty selection.
pub fn char_range_to_byte_range(text: &str, start: i32, end: i32) -> Option<Range<usize>> {
    if start < 0 || end < start {
        return None;
    }
    let start = usize::try_from(start).ok()?;
    // `end` is allowed to be absurd; the clamp below is what bounds it.
    let end = usize::try_from(end).unwrap_or(usize::MAX);
    let total = text.chars().count();
    if start > total {
        return None;
    }

    let mut start_byte = None;
    // A range that runs to the end keeps this default; so does a caret sitting
    // exactly at the end of the text.
    let mut end_byte = text.len();
    for (index, (offset, _)) in text.char_indices().enumerate() {
        if index == start {
            start_byte = Some(offset);
        }
        if index == end {
            end_byte = offset;
            break;
        }
    }
    Some(start_byte.unwrap_or(text.len())..end_byte)
}

/// A caret or selection range, in characters.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TextSelection {
    pub start: i32,
    pub end: i32,
}

impl TextSelection {
    pub const fn new(start: i32, end: i32) -> Self {
        Self { start, end }
    }

    /// A range that selects nothing: the cursor is somewhere, no text is marked.
    pub const fn is_empty(&self) -> bool {
        self.end <= self.start
    }
}

/// How many selections are inspected before giving up.
///
/// Multi-cursor editing is real but rare; a toolkit that reports thousands of
/// selections is either confused or hostile, and each entry costs a round trip.
pub const MAX_SELECTION_ATTEMPTS: i32 = 4;

/// Pick the first non-empty selection out of `count`, asking `read` for each.
///
/// Empty ranges are skipped rather than returned: a zero-width selection means
/// "the caret is here", which is not text, and reporting it as a result would
/// make an empty read look like a successful one. A range with a negative start
/// or an end before its start is malformed and skipped for the same reason.
pub fn choose_selection(
    count: i32,
    mut read: impl FnMut(i32) -> Option<TextSelection>,
) -> Option<TextSelection> {
    if count <= 0 {
        return None;
    }
    for index in 0..count.min(MAX_SELECTION_ATTEMPTS) {
        let Some(selection) = read(index) else {
            continue;
        };
        if selection.start < 0 || selection.is_empty() {
            continue;
        }
        return Some(selection);
    }
    None
}

/// Turn `GetRangeExtents`'s `(iiii)` into a rectangle.
///
/// A non-positive width or height is refused: toolkits return
/// `(0, 0, 0, 0)` for a widget that is not realized, and a zero-size anchor
/// would place a bubble at the top-left corner of the screen. The origin is
/// **not** clamped -- a monitor left of or above the primary one has negative
/// desktop coordinates, and clamping those would drag every bubble on such a
/// layout to the primary head.
pub fn extents_to_rect(extents: (i32, i32, i32, i32)) -> Option<Rect> {
    let (x, y, width, height) = extents;
    if width <= 0 || height <= 0 {
        return None;
    }
    Some(Rect::new(x, y, width as u32, height as u32))
}

/// Cut `text` to at most `max_bytes`, never inside a character.
///
/// `&text[..max_bytes]` panics when the cut lands inside a multi-byte sequence,
/// which is the common case for Chinese text and any emoji.
pub fn clamp_text(text: &str, max_bytes: usize) -> &str {
    if text.len() <= max_bytes {
        return text;
    }
    let mut end = max_bytes;
    while end > 0 && !text.is_char_boundary(end) {
        end -= 1;
    }
    &text[..end]
}

/// The node the walk decided to read text from.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Focus {
    /// Focused, and implements `Text`: the node a selection can be read from.
    Text(ObjectRef),
    /// Focused without `Text` -- a window frame, a button, a list row. Kept
    /// only so the caller can still report *what* has focus.
    Plain(ObjectRef),
}

impl Focus {
    pub fn reference(&self) -> &ObjectRef {
        match self {
            Self::Text(reference) | Self::Plain(reference) => reference,
        }
    }
}

/// How much work the search may do.
///
/// Every node visited costs at least one round trip (`GetState`) and the search
/// runs on the capture path, so it is bounded in both breadth and depth. The
/// defaults cover a normal toolkit tree (window, panels, splitter, view,
/// document -- depth around six) with room to spare.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WalkLimits {
    pub max_visits: usize,
    pub max_depth: u32,
}

pub const DEFAULT_WALK_LIMITS: WalkLimits = WalkLimits {
    max_visits: 256,
    max_depth: 24,
};

/// The three reads the search needs from a tree, in the order it makes them.
///
/// Deliberately a trait rather than a concrete bus client: the search order,
/// the budgets and the cycle guard are the parts that need testing, and they
/// need no bus to be tested.
pub trait AccessibleSource {
    fn children(&mut self, node: &ObjectRef) -> Vec<ObjectRef>;
    fn states(&mut self, node: &ObjectRef) -> Vec<u32>;
    fn interfaces(&mut self, node: &ObjectRef) -> Vec<String>;
}

/// Find the focused node under `roots`.
///
/// Breadth-first, so that a focused window frame is seen before the text field
/// deep inside a sibling subtree: the first focused node that also has `Text`
/// wins, and the first focused node without it is remembered as a fallback. A
/// depth-first search would descend into a stale branch first and spend the
/// whole budget there.
///
/// Cycles are real in these trees -- `Parent` points back up and some toolkits
/// report a child twice -- so references are visited once. A defunct node ends
/// its own branch.
pub fn find_focused(
    source: &mut impl AccessibleSource,
    roots: &[ObjectRef],
    limits: WalkLimits,
) -> Option<Focus> {
    let mut queue: VecDeque<(ObjectRef, u32)> = roots
        .iter()
        .filter(|root| !root.is_null())
        .map(|root| (root.clone(), 0))
        .collect();
    let mut seen: HashSet<(String, String)> = HashSet::new();
    let mut visits = 0usize;
    let mut fallback = None;

    while let Some((node, depth)) = queue.pop_front() {
        if !seen.insert((node.name.clone(), node.path.clone())) {
            continue;
        }
        if visits >= limits.max_visits {
            break;
        }
        visits += 1;

        let states = source.states(&node);
        if is_defunct(&states) {
            continue;
        }
        if has_state(&states, state::FOCUSED) {
            if supports_text(&source.interfaces(&node)) {
                return Some(Focus::Text(node));
            }
            if fallback.is_none() {
                fallback = Some(Focus::Plain(node.clone()));
            }
        }
        if depth >= limits.max_depth {
            continue;
        }
        for child in source.children(&node) {
            if child.is_null() {
                continue;
            }
            queue.push_back((node.child(child), depth + 1));
        }
    }
    fallback
}

/// The application whose process is `pid`, out of the desktop's children.
///
/// Matching by process id is what turns "some application has a focused text
/// field" into "the application the user is looking at does". The caller reads
/// the pid from `_NET_WM_PID` on the active X11 window and resolves each
/// application's bus name to a pid with
/// `org.freedesktop.DBus.GetConnectionUnixProcessID`.
///
/// `pid == 0` is "unknown" and matches nothing: without this, a window that does
/// not publish `_NET_WM_PID` would match whichever application also failed to
/// report one.
pub fn match_app_by_pid(apps: &[(ObjectRef, u32)], pid: u32) -> Option<ObjectRef> {
    if pid == 0 || pid == u32::MAX {
        return None;
    }
    apps.iter()
        .find(|(_, candidate)| *candidate == pid)
        .map(|(app, _)| app.clone())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    // ---- references -----------------------------------------------------

    #[test]
    fn an_empty_bus_name_is_inherited_from_the_parent() {
        let app = ObjectRef::new(":1.42", "/org/a11y/atspi/accessible/root");
        let child = app.child(ObjectRef::new("", "/org/a11y/atspi/accessible/12"));
        assert_eq!(child.name, ":1.42");
        assert_eq!(child.path, "/org/a11y/atspi/accessible/12");

        // A name that is present is kept, not overwritten.
        let other = app.child(ObjectRef::new(":1.99", "/org/a11y/atspi/accessible/3"));
        assert_eq!(other.name, ":1.99");
    }

    #[test]
    fn both_null_forms_are_recognised() {
        assert!(ObjectRef::new(":1.42", NULL_PATH).is_null());
        assert!(ObjectRef::new(":1.42", "").is_null());
        assert!(!ObjectRef::desktop().is_null());
    }

    // ---- state and interface names --------------------------------------

    #[test]
    fn state_values_match_the_atspi_enum() {
        // `AtspiStateType` order in atspi-constants.h: DEFUNCT is the 7th
        // entry, FOCUSED the 13th, SELECTABLE_TEXT the 39th.
        assert_eq!(state::DEFUNCT, 6);
        assert_eq!(state::FOCUSED, 12);
        assert_eq!(state::SELECTABLE_TEXT, 38);
    }

    #[test]
    fn states_are_membership_tested_not_masked() {
        // A bitmask reading of `[FOCUSED]` would be `1 << 12`, which shares no
        // bit with the value the interface actually reports.
        assert!(has_state(&[11, 12, 25], state::FOCUSED));
        assert!(!has_state(&[1 << state::FOCUSED], state::FOCUSED));
        assert!(!has_state(&[], state::FOCUSED));
        assert!(is_defunct(&[6]));
        assert!(!is_defunct(&[12]));
    }

    #[test]
    fn the_text_interface_must_match_exactly() {
        let exact = [
            "org.a11y.atspi.Component".to_string(),
            TEXT_INTERFACE.to_string(),
        ];
        assert!(supports_text(&exact));
        // A prefix match would accept this and then fail on the first method.
        let other = ["org.a11y.atspi.TextInput".to_string()];
        assert!(!supports_text(&other));
        assert!(!supports_text(&[]));
    }

    // ---- character offsets ----------------------------------------------

    #[test]
    fn offsets_are_characters_not_bytes() {
        let text = "中文abc";
        // The second and third characters, which are bytes 3..7.
        assert_eq!(char_range_to_byte_range(text, 1, 3), Some(3..7));
        assert_eq!(&text[3..7], "文a");
        // The whole string.
        assert_eq!(char_range_to_byte_range(text, 0, 5), Some(0..9));
    }

    #[test]
    fn an_astral_character_counts_as_one() {
        let text = "a😀b";
        // Two UTF-16 units, four bytes, one code point.
        assert_eq!(char_range_to_byte_range(text, 1, 2), Some(1..5));
        assert_eq!(&text[1..5], "😀");
    }

    #[test]
    fn a_range_ending_past_the_text_is_clamped() {
        let text = "abc";
        assert_eq!(char_range_to_byte_range(text, 1, 99), Some(1..3));
        assert_eq!(char_range_to_byte_range(text, 0, i32::MAX), Some(0..3));
    }

    #[test]
    fn a_range_starting_past_the_text_is_refused() {
        let text = "abc";
        assert_eq!(char_range_to_byte_range(text, 4, 5), None);
        assert_eq!(char_range_to_byte_range("", 1, 2), None);
        // The empty text at offset zero is an empty range, not a refusal.
        assert_eq!(char_range_to_byte_range("", 0, 0), Some(0..0));
    }

    #[test]
    fn negative_and_reversed_ranges_are_refused() {
        assert_eq!(char_range_to_byte_range("abc", -1, 2), None);
        assert_eq!(char_range_to_byte_range("abc", 2, 1), None);
    }

    #[test]
    fn a_zero_width_range_is_the_caret() {
        let text = "中文abc";
        assert_eq!(char_range_to_byte_range(text, 2, 2), Some(6..6));
        // At the very end of the text.
        assert_eq!(char_range_to_byte_range(text, 5, 5), Some(9..9));
    }

    // ---- selection choice ------------------------------------------------

    #[test]
    fn the_first_non_empty_selection_wins() {
        let asked = std::cell::RefCell::new(Vec::new());
        let chosen = choose_selection(3, |index| {
            asked.borrow_mut().push(index);
            match index {
                0 => Some(TextSelection::new(4, 4)),
                1 => Some(TextSelection::new(2, 8)),
                _ => Some(TextSelection::new(9, 12)),
            }
        });
        assert_eq!(chosen, Some(TextSelection::new(2, 8)));
        // It stops asking as soon as it has an answer.
        assert_eq!(*asked.borrow(), vec![0, 1]);
    }

    #[test]
    fn a_caret_only_text_reports_no_selection() {
        assert_eq!(
            choose_selection(1, |_| Some(TextSelection::new(7, 7))),
            None
        );
        assert_eq!(choose_selection(0, |_| unreachable!()), None);
        assert_eq!(choose_selection(-1, |_| unreachable!()), None);
    }

    #[test]
    fn malformed_and_failing_selections_are_skipped() {
        let chosen = choose_selection(3, |index| match index {
            0 => None,
            1 => Some(TextSelection::new(-2, 5)),
            _ => Some(TextSelection::new(0, 3)),
        });
        assert_eq!(chosen, Some(TextSelection::new(0, 3)));
    }

    #[test]
    fn a_hostile_selection_count_does_not_become_a_round_trip_storm() {
        let asked = std::cell::RefCell::new(0);
        let chosen = choose_selection(10_000, |_| {
            *asked.borrow_mut() += 1;
            Some(TextSelection::new(0, 0))
        });
        assert_eq!(chosen, None);
        assert_eq!(*asked.borrow(), MAX_SELECTION_ATTEMPTS as usize);
    }

    // ---- extents ---------------------------------------------------------

    #[test]
    fn extents_need_a_positive_size() {
        assert_eq!(extents_to_rect((10, 20, 0, 5)), None);
        assert_eq!(extents_to_rect((10, 20, 5, 0)), None);
        assert_eq!(extents_to_rect((0, 0, 0, 0)), None);
        assert_eq!(extents_to_rect((-5, 5, -3, 4)), None);
        assert_eq!(
            extents_to_rect((10, 20, 30, 40)),
            Some(Rect::new(10, 20, 30, 40))
        );
    }

    #[test]
    fn a_negative_origin_is_kept() {
        // A head placed left of the primary one has negative coordinates, and
        // dragging every bubble back to the primary would be visibly wrong.
        assert_eq!(
            extents_to_rect((-1920, -200, 100, 40)),
            Some(Rect::new(-1920, -200, 100, 40))
        );
    }

    // ---- text clamping ---------------------------------------------------

    #[test]
    fn clamping_never_splits_a_character() {
        let text = "中文abc";
        // 4 bytes would land inside the second character.
        assert_eq!(clamp_text(text, 4), "中");
        assert_eq!(clamp_text(text, 3), "中");
        assert_eq!(clamp_text(text, 0), "");
        assert_eq!(clamp_text(text, 9), text);
        assert_eq!(clamp_text(text, 100), text);
    }

    // ---- the walk --------------------------------------------------------

    /// A tree built from tables, counting how many times it was read.
    #[derive(Default)]
    struct FakeTree {
        children: HashMap<(String, String), Vec<ObjectRef>>,
        states: HashMap<(String, String), Vec<u32>>,
        interfaces: HashMap<(String, String), Vec<String>>,
        reads: usize,
    }

    impl FakeTree {
        fn node(
            mut self,
            reference: ObjectRef,
            states: &[u32],
            interfaces: &[&str],
            children: &[ObjectRef],
        ) -> Self {
            let key = key(&reference);
            self.states.insert(key.clone(), states.to_vec());
            self.interfaces.insert(
                key.clone(),
                interfaces.iter().map(|s| s.to_string()).collect(),
            );
            self.children.insert(key, children.to_vec());
            self
        }
    }

    impl AccessibleSource for FakeTree {
        fn children(&mut self, node: &ObjectRef) -> Vec<ObjectRef> {
            self.reads += 1;
            self.children.get(&key(node)).cloned().unwrap_or_default()
        }

        fn states(&mut self, node: &ObjectRef) -> Vec<u32> {
            self.reads += 1;
            self.states.get(&key(node)).cloned().unwrap_or_default()
        }

        fn interfaces(&mut self, node: &ObjectRef) -> Vec<String> {
            self.reads += 1;
            self.interfaces.get(&key(node)).cloned().unwrap_or_default()
        }
    }

    /// The table key for a reference, with the empty bus name resolved the way
    /// the walk resolves it. Looking up by the raw name would make every child
    /// invisible and the tests would pass for the wrong reason.
    fn key(reference: &ObjectRef) -> (String, String) {
        let name = if reference.name.is_empty() {
            ":1.42".to_string()
        } else {
            reference.name.clone()
        };
        (name, reference.path.clone())
    }

    fn app() -> ObjectRef {
        ObjectRef::new(":1.42", "/org/a11y/atspi/accessible/root")
    }

    fn node(path: &str) -> ObjectRef {
        ObjectRef::new("", format!("/org/a11y/atspi/accessible/{path}"))
    }

    /// The reference the walk reports for `path`: same object, bus name filled
    /// in from the application.
    fn inherited(path: &str) -> ObjectRef {
        ObjectRef::new(":1.42", format!("/org/a11y/atspi/accessible/{path}"))
    }

    #[test]
    fn the_first_focused_text_node_wins() {
        let frame = node("frame");
        let view = node("text");
        let mut tree = FakeTree::default()
            .node(
                app(),
                &[8],
                &[ACCESSIBLE_INTERFACE],
                std::slice::from_ref(&frame),
            )
            .node(
                frame.clone(),
                &[25],
                &[ACCESSIBLE_INTERFACE],
                std::slice::from_ref(&view),
            )
            .node(
                view.clone(),
                &[12, 38],
                &[ACCESSIBLE_INTERFACE, TEXT_INTERFACE],
                &[],
            );

        let found = find_focused(&mut tree, &[app()], DEFAULT_WALK_LIMITS);
        match found {
            Some(Focus::Text(reference)) => {
                // The empty bus name was inherited, otherwise the read would be
                // addressed to nobody.
                assert_eq!(reference.name, ":1.42");
                assert_eq!(reference.path, view.path);
            }
            other => panic!("expected the text node, got {other:?}"),
        }
    }

    #[test]
    fn a_focus_without_text_is_only_a_fallback() {
        // Focused frame first, focused text field later: the text field wins
        // even though the frame was seen first.
        let frame = node("frame");
        let entry = node("entry");
        let mut tree = FakeTree::default()
            .node(app(), &[], &[], std::slice::from_ref(&frame))
            .node(frame.clone(), &[12], &[ACCESSIBLE_INTERFACE], &[])
            .node(entry.clone(), &[12], &[TEXT_INTERFACE], &[]);

        // `entry` is not reachable from `app` in this table, so the fallback is
        // what the walk has to report.
        assert_eq!(
            find_focused(&mut tree, &[app()], DEFAULT_WALK_LIMITS),
            Some(Focus::Plain(inherited("frame")))
        );

        // With a path to it, the text node wins.
        let mut tree = FakeTree::default()
            .node(app(), &[], &[], &[frame.clone(), entry.clone()])
            .node(frame.clone(), &[12], &[ACCESSIBLE_INTERFACE], &[])
            .node(entry.clone(), &[12], &[TEXT_INTERFACE], &[]);
        assert_eq!(
            find_focused(&mut tree, &[app()], DEFAULT_WALK_LIMITS),
            Some(Focus::Text(inherited("entry")))
        );
    }

    #[test]
    fn nothing_focused_reports_nothing() {
        let mut tree = FakeTree::default().node(app(), &[25], &[], &[node("x")]);
        assert_eq!(find_focused(&mut tree, &[app()], DEFAULT_WALK_LIMITS), None);
    }

    #[test]
    fn a_defunct_subtree_is_not_descended_into() {
        let dead = node("dead");
        let deep = node("deep");
        let mut tree = FakeTree::default()
            .node(app(), &[], &[], std::slice::from_ref(&dead))
            .node(
                dead.clone(),
                &[state::DEFUNCT],
                &[],
                std::slice::from_ref(&deep),
            )
            .node(deep.clone(), &[12], &[TEXT_INTERFACE], &[]);

        assert_eq!(find_focused(&mut tree, &[app()], DEFAULT_WALK_LIMITS), None);
        // Two reads for the root (states, children) and one for the defunct
        // node: its children were never asked for, so `deep` was never seen.
        assert_eq!(tree.reads, 3, "reads: {}", tree.reads);
    }

    #[test]
    fn cycles_terminate() {
        // A parent that lists itself as a child, and two nodes that list each
        // other, are both shapes real toolkits produce.
        let a = node("a");
        let b = node("b");
        let mut tree = FakeTree::default()
            .node(app(), &[], &[], std::slice::from_ref(&a))
            .node(a.clone(), &[], &[], &[b.clone(), app()])
            .node(b.clone(), &[], &[], std::slice::from_ref(&a));

        assert_eq!(find_focused(&mut tree, &[app()], DEFAULT_WALK_LIMITS), None);
        // Root, a, b: three nodes, and the repeat visit was refused before any
        // read happened.
        assert!(tree.reads <= 7, "{} reads", tree.reads);
    }

    #[test]
    fn the_visit_budget_bounds_the_reads() {
        // A chain far longer than the budget.
        let mut tree = FakeTree::default();
        let references: Vec<ObjectRef> = (0..64).map(|index| node(&index.to_string())).collect();
        for (index, reference) in references.iter().enumerate() {
            let children = references
                .get(index + 1)
                .cloned()
                .into_iter()
                .collect::<Vec<_>>();
            tree = tree.node(reference.clone(), &[], &[], &children);
        }
        let limits = WalkLimits {
            max_visits: 4,
            max_depth: 24,
        };
        assert_eq!(find_focused(&mut tree, &references[..1], limits), None);
        assert!(tree.reads <= 8, "{} reads", tree.reads);
    }

    #[test]
    fn the_depth_budget_stops_the_descent() {
        let first = node("0");
        let second = node("1");
        let third = node("2");
        let mut tree = FakeTree::default()
            .node(app(), &[], &[], std::slice::from_ref(&first))
            .node(first.clone(), &[], &[], std::slice::from_ref(&second))
            .node(second.clone(), &[], &[], std::slice::from_ref(&third))
            .node(third.clone(), &[12], &[TEXT_INTERFACE], &[]);

        let too_shallow = WalkLimits {
            max_visits: 256,
            max_depth: 1,
        };
        assert_eq!(find_focused(&mut tree, &[app()], too_shallow), None);

        let mut tree = FakeTree::default()
            .node(app(), &[], &[], std::slice::from_ref(&first))
            .node(first, &[], &[], std::slice::from_ref(&second))
            .node(second, &[], &[], std::slice::from_ref(&third))
            .node(third, &[12], &[TEXT_INTERFACE], &[]);
        assert_eq!(
            find_focused(&mut tree, &[app()], DEFAULT_WALK_LIMITS),
            Some(Focus::Text(inherited("2")))
        );
    }

    #[test]
    fn a_null_root_is_skipped() {
        let mut tree = FakeTree::default();
        assert_eq!(
            find_focused(
                &mut tree,
                &[ObjectRef::new(":1.42", NULL_PATH)],
                DEFAULT_WALK_LIMITS
            ),
            None
        );
        assert_eq!(tree.reads, 0);
    }

    // ---- application matching -------------------------------------------

    #[test]
    fn an_application_is_matched_by_process_id() {
        let apps = [
            (ObjectRef::new(":1.1", ROOT_PATH), 900),
            (ObjectRef::new(":1.2", ROOT_PATH), 1234),
        ];
        assert_eq!(
            match_app_by_pid(&apps, 1234),
            Some(ObjectRef::new(":1.2", ROOT_PATH))
        );
        assert_eq!(match_app_by_pid(&apps, 4321), None);
        // Unknown pids must not match an application that also reports 0.
        let unknown = [(ObjectRef::new(":1.3", ROOT_PATH), 0)];
        assert_eq!(match_app_by_pid(&unknown, 0), None);
        assert_eq!(match_app_by_pid(&unknown, u32::MAX), None);
    }
}
