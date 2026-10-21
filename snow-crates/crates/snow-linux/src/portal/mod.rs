//! `xdg-desktop-portal` ScreenCast: the negotiation half.
//!
//! Wayland gives no client a copy of the screen. The only supported way to get
//! pixels is to ask the compositor, through the portal, for a PipeWire stream;
//! everything the compositor agrees to is decided during a three-call
//! handshake. This module owns that handshake and nothing after it -- turning
//! the negotiated stream into frames is PipeWire's job and is not implemented.
//!
//! The split is the same one used throughout this crate, for the same reason:
//! answer correctness is separated from plumbing so that most of it can be
//! tested on a host that is not running the thing being tested.
//!
//! * What is here is pure: the numbers the protocol uses, the object path a
//!   reply will arrive on, which options a given portal version accepts, how a
//!   reply is judged, and the order the three calls have to happen in. It talks
//!   to the portal through [`PortalTransport`], which a test can fake.
//! * [`dbus`] is the real transport. It needs a session bus and a running
//!   portal, so it is type-checked rather than executed on the development
//!   machine -- same status as the NEON kernels in `snow-capture`.
//!
//! Two protocol facts drive most of the code below.
//!
//! **Every call is answered asynchronously.** `CreateSession`, `SelectSources`
//! and `Start` all return immediately with a `Request` object path, and the
//! actual answer arrives later as a `Response` signal on that path. The path is
//! predictable ([`request_path`]) precisely so a client can subscribe before
//! calling, which is what avoids the race where a fast portal answers before
//! the subscription exists.
//!
//! **The portal is versioned and the versions add options.** Sending an option
//! a running portal does not know is not a fatal error -- the portal ignores
//! unknown keys -- which is exactly why a silently dropped `restore_token`
//! would be so hard to notice: the handshake succeeds, the permission prompt
//! appears every single time, and nothing in the logs says why. [`select_sources_options`]
//! therefore refuses to build a request that needs a version it was not given.

pub mod deadline;

#[cfg(target_os = "linux")]
pub mod dbus;

use std::time::Duration;

use crate::session::SessionKind;

/// The portal's well-known bus name. One service serves every portal interface.
pub const DESKTOP_SERVICE: &str = "org.freedesktop.portal.Desktop";
/// The one object every portal interface lives on.
pub const DESKTOP_PATH: &str = "/org/freedesktop/portal/desktop";
pub const SCREENCAST_INTERFACE: &str = "org.freedesktop.portal.ScreenCast";
/// The placeholder object each call hands back; replaced by a real path per call.
pub const REQUEST_INTERFACE: &str = "org.freedesktop.portal.Request";
pub const SESSION_INTERFACE: &str = "org.freedesktop.portal.Session";
/// Prefix of every request path, before the sender and the token.
pub const REQUEST_PATH_PREFIX: &str = "/org/freedesktop/portal/desktop/request";

/// `SelectSources.types` bits.
pub const SOURCE_MONITOR: u32 = 1;
pub const SOURCE_WINDOW: u32 = 2;
pub const SOURCE_VIRTUAL: u32 = 4;

/// `SelectSources.cursor_mode` values.
pub const CURSOR_HIDDEN: u32 = 1;
pub const CURSOR_EMBEDDED: u32 = 2;
pub const CURSOR_METADATA: u32 = 4;

/// The interface version this module was written against. Not a requirement --
/// every version check below uses the number the portal reports.
pub const KNOWN_VERSION: u32 = 6;

/// Version that introduced `SelectSources.cursor_mode`.
pub const CURSOR_MODE_SINCE: u32 = 2;
/// Version that introduced `SelectSources.restore_token` and `persist_mode`.
pub const PERSISTENCE_SINCE: u32 = 4;
/// Version that added `source_type` to each stream's properties.
pub const SOURCE_TYPE_SINCE: u32 = 3;
/// Version that added `pipewire-serial`, and deprecated addressing a stream by
/// node id.
pub const PIPEWIRE_SERIAL_SINCE: u32 = 6;

/// Where the compositor should put the pointer.
///
/// The choice is not cosmetic: on Wayland this is the *only* way the pointer can
/// reach a screenshot. There is no client-side pointer position to sample --
/// `XQueryPointer` under XWayland reports the X11 copy, which the compositor
/// does not keep in sync -- so "hidden, and drawn by us" is not available.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CursorMode {
    Hidden,
    /// The compositor draws the pointer into the frame. One source of truth, at
    /// the cost of the pointer being part of the pixels.
    Embedded,
    /// The pointer is left out of the pixels and its position arrives as stream
    /// metadata, which is what a recorder that draws its own highlight wants.
    /// Needs PipeWire metadata consumption, so nothing requests it yet.
    Metadata,
}

impl CursorMode {
    pub const fn raw(self) -> u32 {
        match self {
            Self::Hidden => CURSOR_HIDDEN,
            Self::Embedded => CURSOR_EMBEDDED,
            Self::Metadata => CURSOR_METADATA,
        }
    }

    /// Every cursor mode is version 2 or later, so the gating is shared.
    pub const fn since(self) -> u32 {
        CURSOR_MODE_SINCE
    }
}

/// How long the portal should remember the user's permission for this app.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PersistMode {
    /// Ask again next time.
    DoNot = 0,
    /// Remember until the user revokes it.
    Application = 1,
    /// Remember for this session only.
    Session = 2,
}

impl PersistMode {
    pub const fn raw(self) -> u32 {
        self as u32
    }
}

/// What to ask the compositor for.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SelectOptions {
    /// Bitmask of [`SOURCE_MONITOR`], [`SOURCE_WINDOW`], [`SOURCE_VIRTUAL`].
    pub types: u32,
    /// Whether the user may pick more than one source. A recorder that crops
    /// per monitor wants one stream per request, so this stays false.
    pub multiple: bool,
    pub cursor: CursorMode,
    /// A token from a previous `Start` reply; skips the permission dialog.
    pub restore_token: Option<String>,
    pub persist_mode: Option<PersistMode>,
}

impl SelectOptions {
    /// Ask for every monitor, one stream, pointer embedded.
    pub fn monitors() -> Self {
        Self {
            types: SOURCE_MONITOR,
            multiple: false,
            cursor: CursorMode::Embedded,
            restore_token: None,
            persist_mode: None,
        }
    }
}

/// An option that the running portal is too old to understand.
///
/// Carrying the version numbers is the point: the alternative is a request that
/// succeeds while the option quietly does nothing.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct UnsupportedOption {
    pub option: &'static str,
    /// Version the portal reported.
    pub found: u32,
    /// Version that added the option.
    pub needed: u32,
}

impl std::fmt::Display for UnsupportedOption {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            formatter,
            "portal option {} needs ScreenCast version {}, the running portal is {}",
            self.option, self.needed, self.found
        )
    }
}

/// Build the `SelectSources` options dictionary for a given portal version.
///
/// Returns an error rather than dropping an option the portal cannot honour.
/// The caller then knows whether to ask the user again every time or to rewrite
/// the request.
pub fn select_sources_options(
    version: u32,
    options: &SelectOptions,
) -> Result<Vec<(&'static str, PortalValue)>, UnsupportedOption> {
    let require = |option: &'static str, needed: u32| -> Result<(), UnsupportedOption> {
        if version >= needed {
            Ok(())
        } else {
            Err(UnsupportedOption {
                option,
                found: version,
                needed,
            })
        }
    };

    let mut pairs: Vec<(&'static str, PortalValue)> = Vec::with_capacity(5);
    pairs.push(("types", PortalValue::U32(options.types)));
    pairs.push(("multiple", PortalValue::Bool(options.multiple)));

    require("cursor_mode", options.cursor.since())?;
    pairs.push(("cursor_mode", PortalValue::U32(options.cursor.raw())));

    if options.restore_token.is_some() || options.persist_mode.is_some() {
        require("restore_token", PERSISTENCE_SINCE)?;
    }
    if let Some(token) = &options.restore_token {
        pairs.push(("restore_token", PortalValue::Str(token.clone())));
    }
    if let Some(mode) = options.persist_mode {
        pairs.push(("persist_mode", PortalValue::U32(mode.raw())));
    }

    Ok(pairs)
}

/// Build the object path a request's `Response` signal will arrive on.
///
/// The shape is fixed by the portal: `/org/freedesktop/portal/desktop/request/`
/// followed by the caller's unique bus name with the leading colon dropped and
/// dots replaced by underscores, then the `handle_token` that was sent with the
/// call. Getting the sender mangling wrong means subscribing to a path nothing
/// will ever be emitted on, so the handshake waits until it times out -- which
/// looks exactly like a portal that never answered.
pub fn request_path(sender_unique: &str, token: &str) -> Result<String, HandleTokenError> {
    let Some(sender) = sender_unique.strip_prefix(':') else {
        return Err(HandleTokenError::NotAUniqueName {
            sender: sender_unique.to_string(),
        });
    };
    if sender.is_empty() || !sender.bytes().all(|b| b.is_ascii_digit() || b == b'.') {
        return Err(HandleTokenError::NotAUniqueName {
            sender: sender_unique.to_string(),
        });
    }
    validate_token(token)?;

    Ok(format!(
        "{REQUEST_PATH_PREFIX}/{}/{token}",
        sender.replace('.', "_")
    ))
}

/// A `handle_token` is used as one element of an object path, so it is not free
/// form: object path elements are `[A-Za-z0-9_]` and may not start with a digit.
fn validate_token(token: &str) -> Result<(), HandleTokenError> {
    let mut bytes = token.bytes();
    let starts_well = bytes
        .next()
        .is_some_and(|b| b.is_ascii_alphabetic() || b == b'_');
    if !starts_well || !bytes.all(|b| b.is_ascii_alphanumeric() || b == b'_') {
        return Err(HandleTokenError::InvalidToken {
            token: token.to_string(),
        });
    }
    Ok(())
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum HandleTokenError {
    NotAUniqueName { sender: String },
    InvalidToken { token: String },
}

impl std::fmt::Display for HandleTokenError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::NotAUniqueName { sender } => write!(
                formatter,
                "'{sender}' is not a unique bus name; a portal request path is built from the \
                 caller's :n.m name"
            ),
            Self::InvalidToken { token } => write!(
                formatter,
                "'{token}' cannot be an object path element (letters, digits and '_' only, \
                 not starting with a digit)"
            ),
        }
    }
}

/// The outcome codes a portal `Response` signal may carry.
///
/// The protocol defines three and reserves the rest, so an unknown code is kept
/// as data rather than folded into "failed": a portal that invents a new code
/// is telling us something, and flattening it would hide what.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ResponseCode {
    Success,
    /// The user dismissed the dialog. Not an error: the app must not report it
    /// as one, and must not retry on its own.
    Cancelled,
    Failed(u32),
}

impl ResponseCode {
    pub const fn from_raw(raw: u32) -> Self {
        match raw {
            0 => Self::Success,
            1 => Self::Cancelled,
            other => Self::Failed(other),
        }
    }
}

/// A reply as delivered by the portal: a code plus an `a{sv}` of results.
#[derive(Clone, Debug, PartialEq)]
pub struct PortalResponse {
    pub code: ResponseCode,
    pub results: PortalValue,
}

impl PortalResponse {
    /// Judge the code, keeping the step that produced it in the error.
    fn take_success(self, step: Step) -> Result<PortalValue, PortalError> {
        match self.code {
            ResponseCode::Success => Ok(self.results),
            ResponseCode::Cancelled => Err(PortalError::Cancelled { step }),
            ResponseCode::Failed(code) => Err(PortalError::Denied { step, code }),
        }
    }
}

/// One stream the compositor agreed to send.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ScreenCastStream {
    /// PipeWire node id. Deprecated by portal version 6 in favour of
    /// [`Self::pipewire_serial`], because node ids are recycled after the node
    /// is destroyed -- a stale id can silently attach to somebody else's
    /// stream. Both are kept so the consumer can prefer whichever it has.
    pub node_id: u32,
    /// Pixel size of the stream. Never zero: a stream with no size cannot be
    /// allocated for, and guessing one would produce a buffer the compositor
    /// never fills.
    pub size: (i32, i32),
    /// Position in the compositor's coordinate space, as `(x, y)`.
    pub position: (i32, i32),
    /// `SOURCE_*` value; absent on portal versions before 3.
    pub source_type: Option<u32>,
    /// Stable stream identifier, version 4 and later.
    pub id: Option<String>,
    /// Version 6 replacement for [`Self::node_id`].
    pub pipewire_serial: Option<u64>,
}

impl ScreenCastStream {
    /// Whether two streams of one reply describe the same monitor-sized area.
    pub fn is_empty(&self) -> bool {
        self.size.0 <= 0 || self.size.1 <= 0
    }
}

/// A value read out of a portal reply.
///
/// Deliberately not a general D-Bus value type. Only the shapes the ScreenCast
/// interface actually returns are modelled, and everything else stays
/// [`PortalValue::Opaque`]: a parser that cannot understand a value must treat
/// it as missing, never coerce it.
#[derive(Clone, Debug, PartialEq)]
pub enum PortalValue {
    U32(u32),
    I32(i32),
    U64(u64),
    Bool(bool),
    Str(String),
    ObjectPath(String),
    Array(Vec<PortalValue>),
    Struct(Vec<PortalValue>),
    Dict(Vec<(String, PortalValue)>),
    /// A value this module does not model (any other D-Bus type).
    Opaque,
}

impl PortalValue {
    /// Look up a key in a dictionary. Returns `None` for non-dictionaries.
    pub fn get(&self, key: &str) -> Option<&PortalValue> {
        match self {
            Self::Dict(pairs) => pairs
                .iter()
                .find(|(name, _)| name == key)
                .map(|(_, value)| value),
            _ => None,
        }
    }

    pub fn as_u32(&self) -> Option<u32> {
        match self {
            Self::U32(value) => Some(*value),
            _ => None,
        }
    }

    pub fn as_i32(&self) -> Option<i32> {
        match self {
            Self::I32(value) => Some(*value),
            _ => None,
        }
    }

    pub fn as_u64(&self) -> Option<u64> {
        match self {
            Self::U64(value) => Some(*value),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Self::Str(value) | Self::ObjectPath(value) => Some(value),
            _ => None,
        }
    }

    /// A `(ii)` pair, which is how the portal reports sizes and positions.
    fn as_i32_pair(&self) -> Option<(i32, i32)> {
        match self {
            Self::Struct(fields) if fields.len() == 2 => {
                Some((fields[0].as_i32()?, fields[1].as_i32()?))
            }
            _ => None,
        }
    }
}

/// Why a reply could not be turned into a value this module can act on.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ReplyError {
    /// A key the reply must have is absent.
    MissingKey { key: &'static str },
    /// A key is present but holds another type.
    WrongType {
        key: &'static str,
        expected: &'static str,
    },
    /// `streams` was not an array, or an entry was not `(u a{sv})`.
    MalformedStreams { detail: &'static str },
    /// A stream reported a size that cannot be allocated for.
    InvalidSize { width: i32, height: i32 },
    /// `Start` succeeded but described no streams at all.
    NoStreams,
}

impl std::fmt::Display for ReplyError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::MissingKey { key } => write!(formatter, "reply has no '{key}'"),
            Self::WrongType { key, expected } => {
                write!(formatter, "'{key}' is not {expected}")
            }
            Self::MalformedStreams { detail } => {
                write!(formatter, "malformed stream list: {detail}")
            }
            Self::InvalidSize { width, height } => {
                write!(
                    formatter,
                    "stream reports an unusable size {width}x{height}"
                )
            }
            Self::NoStreams => write!(formatter, "the portal started a session with no streams"),
        }
    }
}

/// Read `session_handle` out of a `CreateSession` reply.
///
/// The type is `s` by specification-of-record, not `o`: the interface documents
/// the out argument as an object path, but the result was shipped as a string
/// and every implementation kept it that way. Both are accepted, because a
/// portal is free to fix its own bug and a client should not care.
pub fn session_handle(results: &PortalValue) -> Result<String, ReplyError> {
    let value = results
        .get("session_handle")
        .ok_or(ReplyError::MissingKey {
            key: "session_handle",
        })?;
    let handle = value.as_str().ok_or(ReplyError::WrongType {
        key: "session_handle",
        expected: "a string or object path",
    })?;
    if !handle.starts_with('/') {
        return Err(ReplyError::WrongType {
            key: "session_handle",
            expected: "an object path",
        });
    }
    Ok(handle.to_string())
}

/// Read the stream list out of a `Start` reply.
pub fn parse_streams(results: &PortalValue) -> Result<Vec<ScreenCastStream>, ReplyError> {
    let entry = results
        .get("streams")
        .ok_or(ReplyError::MissingKey { key: "streams" })?;
    let PortalValue::Array(entries) = entry else {
        return Err(ReplyError::MalformedStreams {
            detail: "streams is not an array",
        });
    };

    let mut streams = Vec::with_capacity(entries.len());
    for each in entries {
        let PortalValue::Struct(fields) = each else {
            return Err(ReplyError::MalformedStreams {
                detail: "an entry is not a (u a{sv}) pair",
            });
        };
        if fields.len() != 2 {
            return Err(ReplyError::MalformedStreams {
                detail: "an entry does not have two fields",
            });
        }
        let node_id = fields[0].as_u32().ok_or(ReplyError::MalformedStreams {
            detail: "a stream's node id is not a u32",
        })?;
        let properties = &fields[1];
        if !matches!(properties, PortalValue::Dict(_)) {
            return Err(ReplyError::MalformedStreams {
                detail: "a stream's properties are not a dictionary",
            });
        }

        let size = properties
            .get("size")
            .ok_or(ReplyError::MissingKey { key: "size" })?
            .as_i32_pair()
            .ok_or(ReplyError::WrongType {
                key: "size",
                expected: "an (ii) pair",
            })?;
        if size.0 <= 0 || size.1 <= 0 {
            return Err(ReplyError::InvalidSize {
                width: size.0,
                height: size.1,
            });
        }

        // `position` is documented as always present, but a compositor that
        // presents a single stream has nothing to position it against. Missing
        // means the origin, which for one stream is the same thing.
        let position = properties
            .get("position")
            .and_then(PortalValue::as_i32_pair)
            .unwrap_or((0, 0));

        streams.push(ScreenCastStream {
            node_id,
            size,
            position,
            source_type: properties.get("source_type").and_then(PortalValue::as_u32),
            id: properties
                .get("id")
                .and_then(|v| v.as_str())
                .map(str::to_string),
            pipewire_serial: properties
                .get("pipewire-serial")
                .and_then(PortalValue::as_u64),
        });
    }

    if streams.is_empty() {
        return Err(ReplyError::NoStreams);
    }
    Ok(streams)
}

/// Which call in the handshake produced a failure.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
    Version,
    CreateSession,
    SelectSources,
    Start,
}

impl Step {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Version => "reading the ScreenCast version",
            Self::CreateSession => "CreateSession",
            Self::SelectSources => "SelectSources",
            Self::Start => "Start",
        }
    }
}

/// Everything that can stop a negotiation.
#[derive(Clone, Debug, PartialEq)]
pub enum PortalError {
    /// No portal, no session bus, or no ScreenCast interface behind it. A
    /// configuration fact, not a fault: callers degrade instead of retrying.
    Unavailable(String),
    TooOld {
        found: u32,
    },
    UnsupportedOption(UnsupportedOption),
    /// The user dismissed the permission dialog.
    Cancelled {
        step: Step,
    },
    /// The portal refused with a code other than 0 or 1.
    Denied {
        step: Step,
        code: u32,
    },
    /// The call itself failed (transport, timeout, malformed reply).
    Failed {
        step: Step,
        detail: String,
    },
    Malformed {
        step: Step,
        error: ReplyError,
    },
}

impl PortalError {
    pub fn step(&self) -> Option<Step> {
        match self {
            Self::Unavailable(_) | Self::TooOld { .. } | Self::UnsupportedOption(_) => None,
            Self::Cancelled { step }
            | Self::Denied { step, .. }
            | Self::Failed { step, .. }
            | Self::Malformed { step, .. } => Some(*step),
        }
    }
}

impl std::fmt::Display for PortalError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Unavailable(detail) => {
                write!(
                    formatter,
                    "xdg-desktop-portal ScreenCast is unavailable: {detail}"
                )
            }
            Self::TooOld { found } => write!(
                formatter,
                "the running portal reports ScreenCast version {found}, and version 1 is the \
                 minimum"
            ),
            Self::UnsupportedOption(option) => write!(formatter, "{option}"),
            Self::Cancelled { step } => {
                write!(
                    formatter,
                    "the user cancelled {} in the portal dialog",
                    step.as_str()
                )
            }
            Self::Denied { step, code } => {
                write!(
                    formatter,
                    "the portal refused {} with response code {code}",
                    step.as_str()
                )
            }
            Self::Failed { step, detail } => {
                write!(formatter, "{} failed: {detail}", step.as_str())
            }
            Self::Malformed { step, error } => {
                write!(
                    formatter,
                    "{} returned an unusable reply: {error}",
                    step.as_str()
                )
            }
        }
    }
}

/// The bus side of the handshake, as the negotiation needs it.
///
/// The transport owns the `handle_token` and the request path it implies,
/// because subscribing to that path has to happen before the call that answers
/// on it -- an ordering the transport can get right and the caller cannot.
pub trait PortalTransport {
    /// The `ScreenCast` interface version, or [`PortalError::Unavailable`] when
    /// there is no such interface.
    fn screencast_version(&mut self) -> Result<u32, PortalError>;

    fn create_session(
        &mut self,
        options: &[(&'static str, PortalValue)],
    ) -> Result<PortalResponse, PortalError>;

    fn select_sources(
        &mut self,
        session: &str,
        options: &[(&'static str, PortalValue)],
    ) -> Result<PortalResponse, PortalError>;

    fn start(
        &mut self,
        session: &str,
        parent_window: &str,
        options: &[(&'static str, PortalValue)],
    ) -> Result<PortalResponse, PortalError>;
}

/// What a completed negotiation produced.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ScreenCastSession {
    pub handle: String,
    pub streams: Vec<ScreenCastStream>,
    /// Present when the portal remembered the permission; feed it back through
    /// [`SelectOptions::restore_token`] to skip the dialog next time.
    pub restore_token: Option<String>,
    /// Version that was negotiated against, kept for diagnostics.
    pub version: u32,
}

/// What to ask for, and how long to wait for each answer.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NegotiateOptions {
    pub select: SelectOptions,
    /// Identifier of the window the dialog should be parented to. Empty is
    /// allowed and means "no parent": the dialog is centred by the portal.
    pub parent_window: String,
}

impl NegotiateOptions {
    pub fn monitors() -> Self {
        Self {
            select: SelectOptions::monitors(),
            parent_window: String::new(),
        }
    }
}

/// How long the real transport waits for one `Response` signal before giving up.
///
/// The portal answers from a dialog that a person is looking at, so this is not
/// a network timeout -- but a portal that crashed mid-request never sends a
/// `Response` at all, and without a bound the capture call would hang forever.
pub const RESPONSE_TIMEOUT: Duration = Duration::from_secs(120);

/// Run the handshake.
///
/// The order is fixed by the protocol: a session is created, the sources for it
/// are selected once, and only then is it started. A failure at any step leaves
/// the session open on the portal side until the connection drops -- deliberately
/// not "closed defensively", because the portal closes a session whose client
/// went away and a `Close` call to a portal that is already failing adds a
/// second failure with no information in it.
pub fn negotiate(
    transport: &mut dyn PortalTransport,
    options: &NegotiateOptions,
) -> Result<ScreenCastSession, PortalError> {
    let version = transport.screencast_version()?;
    if version < 1 {
        return Err(PortalError::TooOld { found: version });
    }

    let select =
        select_sources_options(version, &options.select).map_err(PortalError::UnsupportedOption)?;

    // `handle_token` and `session_handle_token` are the transport's to add: it
    // has to know the token before it can subscribe to the reply.
    let created = transport
        .create_session(&[])
        .map_err(|error| with_step(error, Step::CreateSession))?
        .take_success(Step::CreateSession)?;
    let handle = session_handle(&created).map_err(|error| PortalError::Malformed {
        step: Step::CreateSession,
        error,
    })?;

    let selected = transport
        .select_sources(&handle, &select)
        .map_err(|error| with_step(error, Step::SelectSources))?;
    selected.take_success(Step::SelectSources)?;

    // `Start` takes only `handle_token`, which the transport adds.
    let started = transport
        .start(&handle, &options.parent_window, &[])
        .map_err(|error| with_step(error, Step::Start))?
        .take_success(Step::Start)?;

    let streams = parse_streams(&started).map_err(|error| PortalError::Malformed {
        step: Step::Start,
        error,
    })?;
    let restore_token = started
        .get("restore_token")
        .and_then(|value| value.as_str())
        .map(str::to_string);

    Ok(ScreenCastSession {
        handle,
        streams,
        restore_token,
        version,
    })
}

/// Attach the step to an error that was raised without one (the transport does
/// not know which call it was serving).
fn with_step(error: PortalError, step: Step) -> PortalError {
    match error {
        PortalError::Failed { detail, .. } => PortalError::Failed { step, detail },
        PortalError::Malformed { error, .. } => PortalError::Malformed { step, error },
        other => other,
    }
}

/// What capture is possible in this session.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CapturePlan {
    /// Read the X11 root window. Only correct under a real X server: on
    /// Wayland that window belongs to XWayland and holds only X clients, so the
    /// pixels are a nearly empty desktop with no indication that anything is
    /// missing.
    X11Root,
    Unsupported(UnsupportedCapture),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UnsupportedCapture {
    /// Neither `DISPLAY` nor `WAYLAND_DISPLAY`: a headless or CI host.
    NoDisplay,
    /// A Wayland session without the ScreenCast interface. Usually a portal
    /// that is not installed, or a container without the session bus.
    PortalMissing,
    /// The interface answered but reports version 0, which cannot be used.
    PortalTooOld { found: u32 },
    /// Negotiation is implemented; consuming the PipeWire stream is not.
    PortalCaptureUnimplemented { version: u32 },
}

impl UnsupportedCapture {
    /// The message a caller should surface. It has to say what is missing, not
    /// just that something is.
    pub fn message(self) -> String {
        match self {
            Self::NoDisplay => {
                "no display server: neither DISPLAY nor WAYLAND_DISPLAY is set".into()
            }
            Self::PortalMissing => "this Wayland session has no xdg-desktop-portal ScreenCast \
                 interface, so the compositor cannot be asked for the screen"
                .into(),
            Self::PortalTooOld { found } => format!(
                "the running xdg-desktop-portal reports ScreenCast version {found}, which cannot \
                 be negotiated"
            ),
            Self::PortalCaptureUnimplemented { version } => format!(
                "this Wayland session needs xdg-desktop-portal ScreenCast (version {version} is \
                 present) and the PipeWire side of it is not implemented yet"
            ),
        }
    }
}

/// Decide how to capture, given the session and what the portal offers.
///
/// `portal_version` is the result of a non-interactive probe -- reading the
/// interface's `version` property costs one round trip and shows no dialog.
/// `None` means the probe ran and found nothing.
///
/// The one invariant worth stating: **a Wayland session never returns
/// [`CapturePlan::X11Root`]**, no matter what `DISPLAY` says. XWayland makes
/// `DISPLAY` look usable, and reading it produces a screenshot of the X clients
/// only.
pub fn capture_plan(session: SessionKind, portal_version: Option<u32>) -> CapturePlan {
    match session {
        SessionKind::X11 => CapturePlan::X11Root,
        SessionKind::Headless => CapturePlan::Unsupported(UnsupportedCapture::NoDisplay),
        SessionKind::Wayland => match portal_version {
            None => CapturePlan::Unsupported(UnsupportedCapture::PortalMissing),
            Some(version) if version == 0 => {
                CapturePlan::Unsupported(UnsupportedCapture::PortalTooOld { found: version })
            }
            Some(version) => {
                CapturePlan::Unsupported(UnsupportedCapture::PortalCaptureUnimplemented { version })
            }
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SCREEN: (i32, i32) = (2560, 1440);

    fn dict(pairs: Vec<(&str, PortalValue)>) -> PortalValue {
        PortalValue::Dict(
            pairs
                .into_iter()
                .map(|(key, value)| (key.to_string(), value))
                .collect(),
        )
    }

    fn pair(x: i32, y: i32) -> PortalValue {
        PortalValue::Struct(vec![PortalValue::I32(x), PortalValue::I32(y)])
    }

    fn one_stream(value: Vec<(&str, PortalValue)>) -> PortalValue {
        dict(vec![(
            "streams",
            PortalValue::Array(vec![PortalValue::Struct(vec![
                PortalValue::U32(42),
                dict(value),
            ])]),
        )])
    }

    #[test]
    fn a_request_path_follows_the_portals_sender_mangling() {
        assert_eq!(
            request_path(":1.42", "snow_shot_1").unwrap(),
            "/org/freedesktop/portal/desktop/request/1_42/snow_shot_1"
        );
        // The bus assigns longer names to clients that have been around a while.
        assert_eq!(
            request_path(":1.1234", "tok").unwrap(),
            "/org/freedesktop/portal/desktop/request/1_1234/tok"
        );
    }

    #[test]
    fn a_path_that_cannot_exist_is_refused_rather_than_built() {
        // A well-known name is a different namespace; a path built from it is
        // never emitted on.
        assert!(matches!(
            request_path("org.freedesktop.portal.Desktop", "tok"),
            Err(HandleTokenError::NotAUniqueName { .. })
        ));
        assert!(matches!(
            request_path(":1/2", "tok"),
            Err(HandleTokenError::NotAUniqueName { .. })
        ));
        // An element may not start with a digit, contain a slash, or be empty.
        for token in ["1tok", "a/b", "", "tok-en"] {
            assert!(
                matches!(
                    request_path(":1.2", token),
                    Err(HandleTokenError::InvalidToken { .. })
                ),
                "{token} should have been refused"
            );
        }
    }

    #[test]
    fn response_codes_keep_unknown_values_as_data() {
        assert_eq!(ResponseCode::from_raw(0), ResponseCode::Success);
        assert_eq!(ResponseCode::from_raw(1), ResponseCode::Cancelled);
        assert_eq!(ResponseCode::from_raw(2), ResponseCode::Failed(2));
        assert_eq!(ResponseCode::from_raw(9), ResponseCode::Failed(9));
    }

    #[test]
    fn a_version_one_portal_is_never_asked_for_cursor_mode() {
        // cursor_mode is version 2. Dropping it would produce a dialog with a
        // cursor the caller did not choose, so the request is refused instead.
        let error = select_sources_options(1, &SelectOptions::monitors()).unwrap_err();
        assert_eq!(error.option, "cursor_mode");
        assert_eq!(error.needed, CURSOR_MODE_SINCE);
        assert_eq!(error.found, 1);
    }

    #[test]
    fn version_two_gets_cursor_mode_but_not_persistence() {
        let options = SelectOptions {
            restore_token: Some("token".into()),
            ..SelectOptions::monitors()
        };
        let error = select_sources_options(2, &options).unwrap_err();
        assert_eq!(error.option, "restore_token");
        assert_eq!(error.needed, PERSISTENCE_SINCE);

        let plain = select_sources_options(2, &SelectOptions::monitors()).unwrap();
        let keys: Vec<&str> = plain.iter().map(|(key, _)| *key).collect();
        assert_eq!(keys, ["types", "multiple", "cursor_mode"]);
    }

    #[test]
    fn version_four_gets_every_option_in_a_stable_order() {
        let options = SelectOptions {
            restore_token: Some("token".into()),
            persist_mode: Some(PersistMode::Session),
            ..SelectOptions::monitors()
        };
        let pairs = select_sources_options(4, &options).unwrap();
        let keys: Vec<&str> = pairs.iter().map(|(key, _)| *key).collect();
        assert_eq!(
            keys,
            [
                "types",
                "multiple",
                "cursor_mode",
                "restore_token",
                "persist_mode"
            ]
        );
        assert_eq!(pairs[0].1, PortalValue::U32(SOURCE_MONITOR));
        assert_eq!(pairs[1].1, PortalValue::Bool(false));
        assert_eq!(pairs[2].1, PortalValue::U32(CURSOR_EMBEDDED));
        assert_eq!(pairs[3].1, PortalValue::Str("token".into()));
        assert_eq!(pairs[4].1, PortalValue::U32(2));
    }

    #[test]
    fn a_session_handle_is_accepted_as_a_string_and_as_an_object_path() {
        for value in [
            PortalValue::Str("/org/freedesktop/portal/desktop/session/1_42/s".into()),
            PortalValue::ObjectPath("/org/freedesktop/portal/desktop/session/1_42/s".into()),
        ] {
            let results = dict(vec![("session_handle", value)]);
            assert_eq!(
                session_handle(&results).unwrap(),
                "/org/freedesktop/portal/desktop/session/1_42/s"
            );
        }
    }

    #[test]
    fn a_session_handle_that_is_not_a_path_is_refused() {
        let results = dict(vec![("session_handle", PortalValue::Str("s".into()))]);
        assert!(matches!(
            session_handle(&results),
            Err(ReplyError::WrongType { .. })
        ));
        assert!(matches!(
            session_handle(&dict(vec![])),
            Err(ReplyError::MissingKey { .. })
        ));
    }

    #[test]
    fn a_stream_carries_every_property_the_protocol_documents() {
        let results = one_stream(vec![
            ("size", pair(SCREEN.0, SCREEN.1)),
            ("position", pair(2560, 0)),
            ("source_type", PortalValue::U32(SOURCE_MONITOR)),
            ("id", PortalValue::Str("monitor-1".into())),
            ("pipewire-serial", PortalValue::U64(7)),
        ]);
        let streams = parse_streams(&results).unwrap();
        assert_eq!(
            streams,
            vec![ScreenCastStream {
                node_id: 42,
                size: SCREEN,
                position: (2560, 0),
                source_type: Some(SOURCE_MONITOR),
                id: Some("monitor-1".into()),
                pipewire_serial: Some(7),
            }]
        );
    }

    #[test]
    fn a_version_one_reply_has_neither_source_type_nor_serial() {
        let results = one_stream(vec![("size", pair(SCREEN.0, SCREEN.1))]);
        let stream = parse_streams(&results).unwrap().remove(0);
        assert_eq!(stream.source_type, None);
        assert_eq!(stream.pipewire_serial, None);
        // A single stream has nothing to be positioned against.
        assert_eq!(stream.position, (0, 0));
    }

    #[test]
    fn a_stream_without_a_usable_size_is_refused() {
        assert!(matches!(
            parse_streams(&one_stream(vec![])),
            Err(ReplyError::MissingKey { key: "size" })
        ));
        assert!(matches!(
            parse_streams(&one_stream(vec![("size", pair(0, 1440))])),
            Err(ReplyError::InvalidSize { .. })
        ));
        // A negative size is what a compositor underflow looks like; allocating
        // for it would wrap into a huge buffer.
        assert!(matches!(
            parse_streams(&one_stream(vec![("size", pair(-1, 1440))])),
            Err(ReplyError::InvalidSize { .. })
        ));
    }

    #[test]
    fn a_reply_that_is_not_a_stream_list_is_refused() {
        assert!(matches!(
            parse_streams(&dict(vec![("streams", PortalValue::U32(1))])),
            Err(ReplyError::MalformedStreams { .. })
        ));
        assert!(matches!(
            parse_streams(&dict(vec![(
                "streams",
                PortalValue::Array(vec![PortalValue::U32(42)])
            )])),
            Err(ReplyError::MalformedStreams { .. })
        ));
        // One good entry and one bad one fails the whole reply: half a stream
        // list silently loses a monitor.
        assert!(matches!(
            parse_streams(&dict(vec![(
                "streams",
                PortalValue::Array(vec![
                    PortalValue::Struct(vec![
                        PortalValue::U32(1),
                        dict(vec![("size", pair(10, 10))]),
                    ]),
                    PortalValue::Struct(vec![PortalValue::U32(2)]),
                ])
            )])),
            Err(ReplyError::MalformedStreams { .. })
        ));
    }

    #[test]
    fn an_empty_stream_list_is_a_failure_not_an_empty_screen() {
        assert_eq!(
            parse_streams(&dict(vec![("streams", PortalValue::Array(Vec::new()))])).unwrap_err(),
            ReplyError::NoStreams
        );
    }

    /// A transport that answers from a script and records what it was asked.
    ///
    /// This is the whole reason [`PortalTransport`] exists: the handshake's
    /// ordering, option gating and cancellation handling are decided here, and
    /// none of them need a bus to be wrong.
    #[derive(Default)]
    struct FakeTransport {
        version: u32,
        calls: Vec<String>,
        /// Keys of the last `SelectSources` options, in the order they were sent.
        select_keys: Vec<&'static str>,
        start_reply: Option<PortalValue>,
        cancel_at: Option<&'static str>,
    }

    impl FakeTransport {
        fn new(version: u32) -> Self {
            Self {
                version,
                calls: Vec::new(),
                select_keys: Vec::new(),
                start_reply: None,
                cancel_at: None,
            }
        }

        fn reply(
            &mut self,
            step: Step,
            results: PortalValue,
        ) -> Result<PortalResponse, PortalError> {
            // Recorded before the cancel check: a cancelled call was still made,
            // and where the handshake stopped is what the test is about.
            self.calls.push(step.as_str().to_string());
            if self.cancel_at == Some(step.as_str()) {
                return Ok(PortalResponse {
                    code: ResponseCode::Cancelled,
                    results,
                });
            }
            Ok(PortalResponse {
                code: ResponseCode::Success,
                results,
            })
        }
    }

    impl PortalTransport for FakeTransport {
        fn screencast_version(&mut self) -> Result<u32, PortalError> {
            self.calls.push(Step::Version.as_str().to_string());
            Ok(self.version)
        }

        fn create_session(
            &mut self,
            options: &[(&'static str, PortalValue)],
        ) -> Result<PortalResponse, PortalError> {
            assert!(options.is_empty(), "the transport owns the handle tokens");
            self.reply(
                Step::CreateSession,
                dict(vec![(
                    "session_handle",
                    PortalValue::Str("/org/freedesktop/portal/desktop/session/1_2/s".into()),
                )]),
            )
        }

        fn select_sources(
            &mut self,
            session: &str,
            options: &[(&'static str, PortalValue)],
        ) -> Result<PortalResponse, PortalError> {
            assert_eq!(session, "/org/freedesktop/portal/desktop/session/1_2/s");
            self.select_keys = options.iter().map(|(key, _)| *key).collect();
            self.reply(Step::SelectSources, dict(vec![]))
        }

        fn start(
            &mut self,
            session: &str,
            parent_window: &str,
            options: &[(&'static str, PortalValue)],
        ) -> Result<PortalResponse, PortalError> {
            assert_eq!(session, "/org/freedesktop/portal/desktop/session/1_2/s");
            assert!(options.is_empty());
            assert_eq!(parent_window, "");
            let results = self.start_reply.clone().unwrap_or_else(|| {
                one_stream(vec![
                    ("size", pair(SCREEN.0, SCREEN.1)),
                    ("source_type", PortalValue::U32(SOURCE_MONITOR)),
                ])
            });
            self.reply(Step::Start, results)
        }
    }

    #[test]
    fn a_negotiation_runs_the_three_calls_in_order() {
        let mut transport = FakeTransport::new(4);
        let session = negotiate(&mut transport, &NegotiateOptions::monitors()).unwrap();

        assert_eq!(
            transport.calls,
            [
                Step::Version.as_str(),
                Step::CreateSession.as_str(),
                Step::SelectSources.as_str(),
                Step::Start.as_str(),
            ]
        );
        assert_eq!(transport.select_keys, ["types", "multiple", "cursor_mode"]);
        assert_eq!(
            session.handle,
            "/org/freedesktop/portal/desktop/session/1_2/s"
        );
        assert_eq!(session.streams.len(), 1);
        assert_eq!(session.streams[0].size, SCREEN);
        assert_eq!(session.version, 4);
    }

    #[test]
    fn a_cancelled_dialog_stops_the_handshake_where_it_happened() {
        for (step, expected_calls) in [
            (
                Step::CreateSession.as_str(),
                vec![
                    Step::Version.as_str(),
                    // The call was made; the answer was "cancelled".
                    Step::CreateSession.as_str(),
                ],
            ),
            (
                Step::SelectSources.as_str(),
                vec![
                    Step::Version.as_str(),
                    Step::CreateSession.as_str(),
                    Step::SelectSources.as_str(),
                ],
            ),
        ] {
            let mut transport = FakeTransport::new(4);
            transport.cancel_at = Some(step);
            let error = negotiate(&mut transport, &NegotiateOptions::monitors()).unwrap_err();
            assert!(
                matches!(error, PortalError::Cancelled { .. }),
                "{step}: {error}"
            );
            assert_eq!(transport.calls, expected_calls);
        }
    }

    #[test]
    fn an_old_portal_fails_before_any_call_is_made() {
        let mut transport = FakeTransport::new(0);
        let error = negotiate(&mut transport, &NegotiateOptions::monitors()).unwrap_err();
        assert_eq!(error, PortalError::TooOld { found: 0 });
        // A version 0 interface is not even worth creating a session on.
        assert_eq!(transport.calls, [Step::Version.as_str()]);
    }

    #[test]
    fn a_version_one_portal_negotiates_without_the_v2_options() {
        let mut transport = FakeTransport::new(1);
        // A version 1 portal cannot honour an embedded cursor, so asking for one
        // must fail before the session is created rather than after.
        let error = negotiate(&mut transport, &NegotiateOptions::monitors()).unwrap_err();
        assert!(
            matches!(error, PortalError::UnsupportedOption(_)),
            "{error}"
        );
        assert_eq!(transport.calls, [Step::Version.as_str()]);
    }

    #[test]
    fn a_denied_code_is_reported_with_the_step_and_the_code() {
        struct Denying;
        impl PortalTransport for Denying {
            fn screencast_version(&mut self) -> Result<u32, PortalError> {
                Ok(4)
            }
            fn create_session(
                &mut self,
                _options: &[(&'static str, PortalValue)],
            ) -> Result<PortalResponse, PortalError> {
                Ok(PortalResponse {
                    code: ResponseCode::Failed(7),
                    results: dict(vec![]),
                })
            }
            fn select_sources(
                &mut self,
                _session: &str,
                _options: &[(&'static str, PortalValue)],
            ) -> Result<PortalResponse, PortalError> {
                unreachable!("a denied CreateSession must stop the handshake")
            }
            fn start(
                &mut self,
                _session: &str,
                _parent_window: &str,
                _options: &[(&'static str, PortalValue)],
            ) -> Result<PortalResponse, PortalError> {
                unreachable!()
            }
        }

        let error = negotiate(&mut Denying, &NegotiateOptions::monitors()).unwrap_err();
        assert_eq!(
            error,
            PortalError::Denied {
                step: Step::CreateSession,
                code: 7
            }
        );
        assert_eq!(error.step(), Some(Step::CreateSession));
    }

    #[test]
    fn a_malformed_start_reply_names_the_step() {
        let mut transport = FakeTransport::new(4);
        transport.start_reply = Some(dict(vec![("streams", PortalValue::U32(0))]));
        let error = negotiate(&mut transport, &NegotiateOptions::monitors()).unwrap_err();
        assert_eq!(
            error,
            PortalError::Malformed {
                step: Step::Start,
                error: ReplyError::MalformedStreams {
                    detail: "streams is not an array"
                }
            }
        );
    }

    #[test]
    fn a_timestamp_from_the_portal_is_carried_through_when_present() {
        let mut transport = FakeTransport::new(4);
        let mut results = one_stream(vec![("size", pair(SCREEN.0, SCREEN.1))]);
        if let PortalValue::Dict(pairs) = &mut results {
            pairs.push(("restore_token".into(), PortalValue::Str("restored".into())));
        }
        transport.start_reply = Some(results);
        let session = negotiate(&mut transport, &NegotiateOptions::monitors()).unwrap();
        assert_eq!(session.restore_token.as_deref(), Some("restored"));
    }

    #[test]
    fn an_x11_session_never_asks_the_portal_for_anything() {
        // The probe result is ignored on X11: reading the root window is exact
        // there, and a portal round trip would add nothing.
        assert_eq!(capture_plan(SessionKind::X11, None), CapturePlan::X11Root);
        assert_eq!(
            capture_plan(SessionKind::X11, Some(6)),
            CapturePlan::X11Root
        );
    }

    #[test]
    fn a_wayland_session_never_falls_back_to_the_root_window() {
        // The load-bearing invariant. XWayland exports DISPLAY, so "there is an
        // X display" is true on a Wayland session and means nothing.
        for version in [None, Some(0), Some(1), Some(6)] {
            assert_ne!(
                capture_plan(SessionKind::Wayland, version),
                CapturePlan::X11Root,
                "portal version {version:?}"
            );
        }
    }

    #[test]
    fn each_wayland_outcome_explains_what_is_missing() {
        assert!(matches!(
            capture_plan(SessionKind::Wayland, None),
            CapturePlan::Unsupported(UnsupportedCapture::PortalMissing)
        ));
        assert!(matches!(
            capture_plan(SessionKind::Wayland, Some(0)),
            CapturePlan::Unsupported(UnsupportedCapture::PortalTooOld { found: 0 })
        ));

        let CapturePlan::Unsupported(reason) = capture_plan(SessionKind::Wayland, Some(6)) else {
            panic!("a Wayland session without frame consumption is not capturable");
        };
        assert_eq!(
            reason,
            UnsupportedCapture::PortalCaptureUnimplemented { version: 6 }
        );
        // The message has to be usable in a UI: it names the missing piece.
        let message = reason.message();
        assert!(message.contains("PipeWire"), "{message}");
        assert!(message.contains('6'), "{message}");

        assert_eq!(
            capture_plan(SessionKind::Headless, None),
            CapturePlan::Unsupported(UnsupportedCapture::NoDisplay)
        );
    }
}
