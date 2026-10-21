//! X11 PRIMARY-selection rules that hold without a connection.
//!
//! Every item here is a pure function of protocol values, so it compiles and is
//! unit tested on the Windows dev box even though only Linux calls it in
//! production. The transport that feeds these values lives in
//! `platform::linux::selection` and contains no decisions of its own.

use crate::model::{ErrorKind, SelectionError};

/// The text targets this crate can decode, most preferred first.
///
/// `COMPOUND_TEXT` is deliberately absent: it is a locale-dependent compound
/// encoding that cannot be decoded without Xlib's converter. An owner that
/// offers nothing else is reported as unsupported instead of being mangled.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Target {
    /// `UTF8_STRING`: the convention every modern toolkit offers first.
    Utf8,
    /// The MIME spelling, which Wayland-aware toolkits also list.
    PlainUtf8,
    /// ICCCM `STRING`: ISO 8859-1, one byte per character.
    Str,
    /// ICCCM `TEXT`: the owner's choice of encoding. See [`decode`].
    Text,
}

pub(crate) const TARGETS: [Target; 4] =
    [Target::Utf8, Target::PlainUtf8, Target::Str, Target::Text];

impl Target {
    /// X atom name. Atom names are case-sensitive, so these must stay exact.
    pub(crate) fn atom_name(self) -> &'static str {
        match self {
            Self::Utf8 => "UTF8_STRING",
            Self::PlainUtf8 => "text/plain;charset=utf-8",
            Self::Str => "STRING",
            Self::Text => "TEXT",
        }
    }

    /// Position in [`TARGETS`], which is also the atom slot `target_plan` indexes.
    pub(crate) fn index(self) -> usize {
        match self {
            Self::Utf8 => 0,
            Self::PlainUtf8 => 1,
            Self::Str => 2,
            Self::Text => 3,
        }
    }
}

/// Targets to try, in order, given the atoms the owner listed for `TARGETS`.
///
/// `atoms[i]` must be the interned atom for `TARGETS[i]`. `offered` is `None`
/// when the owner does not answer `TARGETS` at all, which `xterm`-derived
/// terminals do: probing the whole list is then the only option, because a
/// refused target costs one `ConvertSelection` round trip and changes nothing.
pub(crate) fn target_plan(offered: Option<&[u32]>, atoms: &[u32; TARGETS.len()]) -> Vec<Target> {
    TARGETS
        .into_iter()
        .enumerate()
        .filter(|(index, _)| offered.is_none_or(|list| list.contains(&atoms[*index])))
        .map(|(_, target)| target)
        .collect()
}

/// Turn a property value into text according to the target that produced it.
///
/// Trailing NULs are dropped first: some owners NUL-terminate the property even
/// though the returned length already accounts for it, and a NUL is never part
/// of a selection. Interior NULs are kept for UTF-8 and cannot occur in practice
/// for the other targets.
pub(crate) fn decode(target: Target, bytes: &[u8]) -> Result<String, SelectionError> {
    let end = bytes
        .iter()
        .rposition(|byte| *byte != 0)
        .map_or(0, |index| index + 1);
    let bytes = &bytes[..end];
    match target {
        Target::Utf8 | Target::PlainUtf8 => std::str::from_utf8(bytes)
            .map(str::to_owned)
            .map_err(|_| malformed("UTF8_STRING")),
        // ISO 8859-1 maps byte `n` to U+00`n`, which is exactly what `char::from`
        // does for `u8`. A lossy decode would replace the only byte that matters.
        Target::Str => Ok(bytes.iter().map(|byte| char::from(*byte)).collect()),
        // `TEXT` predates UTF-8 and is documented as the owner's choice, so the
        // order is a heuristic: valid UTF-8 wins, Latin-1 catches the rest.
        // Latin-1 text that happens to be valid UTF-8 would be misread, but that
        // needs a sequence like C3 A9 standing for two Latin-1 characters, which
        // is far rarer than a modern owner sending UTF-8 under the old name.
        Target::Text => Ok(match std::str::from_utf8(bytes) {
            Ok(text) => text.to_owned(),
            Err(_) => bytes.iter().map(|byte| char::from(*byte)).collect(),
        }),
    }
}

/// Append one property value to the accumulator, refusing to grow past `limit`.
///
/// The check runs before the copy, so an owner cannot make the reader allocate
/// more than the caller allowed, however many `INCR` chunks it sends.
pub(crate) fn append_chunk(
    acc: &mut Vec<u8>,
    chunk: &[u8],
    limit: usize,
) -> Result<(), SelectionError> {
    if acc
        .len()
        .checked_add(chunk.len())
        .is_none_or(|total| total > limit)
    {
        return Err(SelectionError::new(
            ErrorKind::LimitExceeded,
            "PRIMARY selection",
        ));
    }
    acc.extend_from_slice(chunk);
    Ok(())
}

/// Continue reading the same property after `bytes` bytes arrived.
///
/// `GetProperty`'s offset counts 4-byte units, not bytes, so the increment is
/// the byte count divided by four. A reply shorter than one unit can only be the
/// last one, and the `max(1)` is there so a broken owner cannot park the reader
/// on the same offset forever.
pub(crate) fn next_offset(offset: u32, bytes: usize) -> Result<u32, SelectionError> {
    let step = u32::try_from(bytes / 4).unwrap_or(u32::MAX).max(1);
    offset
        .checked_add(step)
        .ok_or_else(|| SelectionError::new(ErrorKind::LimitExceeded, "PRIMARY offset"))
}

/// Whether the reader is in the middle of an `INCR` transfer.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) enum Incr {
    #[default]
    Idle,
    Awaiting,
}

/// What one `GetProperty` reply means for the reader.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Fetch {
    /// The property only announces that the value exceeds a property's size.
    StartIncr,
    /// A zero-length `INCR` chunk closes the transfer.
    End,
    /// Payload for this reply; `more` means the same property continues.
    Payload { more: bool },
}

/// Classify a `GetProperty` reply.
///
/// The order of the checks is the part that is easy to get wrong: an `INCR`
/// announcement comes back with format 32 and a 32-bit size estimate as its
/// value, so testing `format == 8` first would reject every large transfer.
pub(crate) fn fetch_step(
    format: u8,
    type_is_incr: bool,
    bytes_after: u32,
    length: usize,
    incr: Incr,
) -> Result<Fetch, SelectionError> {
    if incr == Incr::Idle && type_is_incr {
        return Ok(Fetch::StartIncr);
    }
    if incr == Incr::Awaiting && length == 0 {
        return Ok(Fetch::End);
    }
    if format != 8 {
        return Err(malformed("PRIMARY property format"));
    }
    Ok(Fetch::Payload {
        more: bytes_after > 0,
    })
}

/// Basename of a resolved `/proc/<pid>/exe` link.
///
/// The kernel appends `" (deleted)"` once the binary has been replaced in place,
/// which is exactly what a self-updater does. Leaving the suffix on would make
/// every exclusion list and every reported source name miss the running copy.
pub(crate) fn executable_basename(link_target: &str) -> String {
    let path = link_target
        .strip_suffix(" (deleted)")
        .unwrap_or(link_target);
    path.rsplit('/').next().unwrap_or_default().to_owned()
}

fn malformed(operation: &'static str) -> SelectionError {
    SelectionError::new(ErrorKind::MalformedData, operation)
}

#[cfg(test)]
mod tests {
    use super::*;

    // Deliberately not 1, 2, 3, 4: a plan that returns indices instead of atoms,
    // or atoms instead of indices, has to produce a visibly different answer.
    const ATOMS: [u32; 4] = [0x91, 0x92, 0x93, 0x94];

    #[test]
    fn atom_names_are_spelled_exactly_as_the_owners_list_them() {
        // Atom names are case-sensitive: "Utf8_string" would intern a different
        // atom, and every owner would then refuse the target as unsupported.
        assert_eq!(
            TARGETS.map(Target::atom_name),
            ["UTF8_STRING", "text/plain;charset=utf-8", "STRING", "TEXT"]
        );
    }

    #[test]
    fn plan_without_targets_lists_every_supported_target_in_preference_order() {
        assert_eq!(target_plan(None, &ATOMS), TARGETS.to_vec());
    }

    #[test]
    fn plan_keeps_only_offered_targets_and_orders_them_by_preference() {
        assert_eq!(
            target_plan(Some(&[0x94, 0x91]), &ATOMS),
            vec![Target::Utf8, Target::Text]
        );
        assert_eq!(target_plan(Some(&[0x93]), &ATOMS), vec![Target::Str]);
        // An owner that offers only compound text is unsupported, not empty.
        assert!(target_plan(Some(&[0x99, 0x98]), &ATOMS).is_empty());
    }

    #[test]
    fn utf8_targets_reject_invalid_sequences_instead_of_replacing_them() {
        assert_eq!(decode(Target::Utf8, "中文".as_bytes()).unwrap(), "中文");
        assert_eq!(
            decode(Target::PlainUtf8, b"a\xFFb").unwrap_err().kind,
            ErrorKind::MalformedData
        );
    }

    #[test]
    fn string_target_decodes_latin1_where_utf8_is_invalid() {
        // C3 A9 is a valid UTF-8 "é", but as ICCCM STRING it is two Latin-1
        // characters. Reading it as UTF-8 here would hide a real mismatch.
        assert_eq!(decode(Target::Str, &[0xC3, 0xA9]).unwrap(), "Ã©");
        assert_eq!(decode(Target::Str, b"plain").unwrap(), "plain");
    }

    #[test]
    fn text_target_prefers_utf8_and_falls_back_to_latin1() {
        assert_eq!(decode(Target::Text, "中文".as_bytes()).unwrap(), "中文");
        assert_eq!(decode(Target::Text, &[0xE9, 0x74, 0xE9]).unwrap(), "été");
    }

    #[test]
    fn trailing_nuls_are_dropped_but_the_rest_is_untouched() {
        assert_eq!(decode(Target::Utf8, b"abc\0\0").unwrap(), "abc");
        assert_eq!(decode(Target::Utf8, b"").unwrap(), "");
        assert_eq!(decode(Target::Utf8, b"\0").unwrap(), "");
        // "a\n " is selection content, not padding.
        assert_eq!(decode(Target::Utf8, b"a\n \0").unwrap(), "a\n ");
    }

    #[test]
    fn chunks_are_bounded_before_they_are_copied() {
        let mut acc = Vec::new();
        append_chunk(&mut acc, b"1234", 5).unwrap();
        assert_eq!(acc, b"1234");
        assert_eq!(
            append_chunk(&mut acc, b"56", 5).unwrap_err().kind,
            ErrorKind::LimitExceeded
        );
        assert_eq!(acc, b"1234");
    }

    #[test]
    fn offsets_stop_at_the_thirty_two_bit_boundary() {
        // Eight bytes are two 4-byte units.
        assert_eq!(next_offset(0, 8).unwrap(), 2);
        assert_eq!(
            next_offset(u32::MAX - 1, 8).unwrap_err().kind,
            ErrorKind::LimitExceeded
        );
    }

    #[test]
    fn a_sub_unit_reply_still_advances_the_offset() {
        assert_eq!(next_offset(7, 1).unwrap(), 8);
    }

    #[test]
    fn target_index_matches_the_preference_list() {
        for (index, target) in TARGETS.into_iter().enumerate() {
            assert_eq!(target.index(), index);
        }
    }

    #[test]
    fn incr_announcement_is_recognised_before_the_format_check() {
        assert_eq!(
            fetch_step(32, true, 0, 4, Incr::Idle).unwrap(),
            Fetch::StartIncr
        );
    }

    #[test]
    fn incr_transfer_keeps_reading_until_a_zero_length_chunk() {
        assert_eq!(
            fetch_step(8, false, 0, 16, Incr::Awaiting).unwrap(),
            Fetch::Payload { more: false }
        );
        assert_eq!(
            fetch_step(8, false, 0, 0, Incr::Awaiting).unwrap(),
            Fetch::End
        );
    }

    #[test]
    fn whole_value_reports_more_only_while_bytes_remain() {
        assert_eq!(
            fetch_step(8, false, 4096, 8, Incr::Idle).unwrap(),
            Fetch::Payload { more: true }
        );
        assert_eq!(
            fetch_step(8, false, 0, 8, Incr::Idle).unwrap(),
            Fetch::Payload { more: false }
        );
    }

    #[test]
    fn non_byte_formats_are_rejected_outside_the_incr_handshake() {
        assert_eq!(
            fetch_step(32, false, 0, 4, Incr::Idle).unwrap_err().kind,
            ErrorKind::MalformedData
        );
        assert_eq!(
            fetch_step(16, false, 0, 2, Incr::Awaiting)
                .unwrap_err()
                .kind,
            ErrorKind::MalformedData
        );
    }

    #[test]
    fn replaced_binaries_lose_the_kernel_deleted_suffix() {
        assert_eq!(
            executable_basename("/usr/bin/snow-shot"),
            "snow-shot".to_owned()
        );
        assert_eq!(
            executable_basename("/opt/app/snow-shot (deleted)"),
            "snow-shot".to_owned()
        );
        assert_eq!(executable_basename("/usr/bin/editor"), "editor".to_owned());
        assert_eq!(executable_basename(""), String::new());
    }
}
