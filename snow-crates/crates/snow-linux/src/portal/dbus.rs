//! The real transport: `xdg-desktop-portal` over the session bus.
//!
//! Everything here needs a running session bus and a running portal, so on the
//! development machine it is type-checked for both Linux targets and never
//! executed -- the same status the NEON kernels in `snow-capture` have. What
//! *can* be decided off-Linux (option gating, call order, reply judgement) is
//! in the parent module and covered by tests that do not come near this file.
//!
//! Three things about this layer are worth reading before changing it.
//!
//! **The subscription has to exist before the call.** Every ScreenCast call
//! answers on a `Request` object by emitting `Response`, so a client that calls
//! first and subscribes second loses the answer when the portal is fast (it
//! will be: the answer to `CreateSession` involves no dialog). The path is
//! computed in advance from the caller's unique name and a token this module
//! chooses, which is the only reason the ordering is possible at all.
//!
//! **A portal may still answer on another path.** The token is a request, not a
//! command: the spec has the method's return value carry the path the answer
//! will arrive on. When it differs from the predicted one, the stream is
//! re-subscribed on the returned path rather than failing -- some versions of
//! the portal in the field ignore `handle_token` and pick their own.
//!
//! **A `Response` is not a method reply.** It carries a code (`0` success, `1`
//! cancelled, anything else a refusal), and the payload is an `a{sv}` whose
//! contents differ per call. Nothing about the method's return value says
//! whether the user agreed.

use std::collections::HashMap;
use std::pin::Pin;
use std::time::Duration;

use zbus::export::futures_core::Stream;
use zbus::export::serde::Serialize;
use zbus::message;
use zbus::wire::{ObjectPath, Type, Variant};
use zbus::{Connection, MatchRule, Message, MessageStream};

use super::deadline::timeout_after;
use super::{
    DESKTOP_PATH, DESKTOP_SERVICE, NegotiateOptions, PortalError, PortalResponse, PortalTransport,
    PortalValue, REQUEST_INTERFACE, RESPONSE_TIMEOUT, SCREENCAST_INTERFACE, ScreenCastSession,
    Step, request_path,
};

/// The session bus connection plus the identity the request paths are built
/// from.
///
/// Holding the connection is not incidental: a portal session is closed when
/// the client that created it disconnects, so a negotiated session is only
/// usable while this object is alive. [`PortalSession`] is what keeps the two
/// together.
pub struct BusTransport {
    connection: Connection,
    /// This client's `:n.m` name; the request path prefix comes from it.
    sender: String,
    /// Distinguishes the tokens of consecutive requests.
    counter: u64,
    timeout: Duration,
}

impl BusTransport {
    /// Connect to the session bus.
    ///
    /// No portal call is made here: a missing portal shows up as
    /// [`PortalError::Unavailable`] from [`PortalTransport::screencast_version`],
    /// which is what lets the probe in [`screencast_version`] stay cheap.
    pub fn connect() -> Result<Self, PortalError> {
        let connection = zbus::block_on(Connection::session()).map_err(|error| {
            PortalError::Unavailable(format!("cannot connect to the session bus: {error}"))
        })?;
        let sender = connection
            .unique_name()
            .map(|name| name.as_str().to_string())
            .ok_or_else(|| {
                PortalError::Unavailable(
                    "the bus connection has no unique name, so no request path can be built"
                        .to_string(),
                )
            })?;
        Ok(Self {
            connection,
            sender,
            counter: 0,
            timeout: RESPONSE_TIMEOUT,
        })
    }

    /// A token that is both unique per call and a valid object path element.
    ///
    /// The process id is not a uniqueness claim -- it is a debugging aid: two
    /// requests in one log are easier to tell apart when the token says which
    /// process asked.
    fn next_token(&mut self, prefix: &str) -> String {
        self.counter += 1;
        format!("{prefix}_{}_{}", std::process::id(), self.counter)
    }

    /// Reserve a token and the request path its answer will arrive on.
    ///
    /// Split from [`Self::finish`] because both are needed before the call: the
    /// token goes inside the options dictionary, and the path has to be
    /// subscribed to first. A caller that built its own options would have to
    /// duplicate the token generation to do the same thing.
    fn begin(&mut self, step: Step) -> Result<(String, String), PortalError> {
        let token = self.next_token("snow_shot");
        let path = request_path(&self.sender, &token).map_err(|error| PortalError::Failed {
            step,
            detail: error.to_string(),
        })?;
        Ok((token, path))
    }

    /// The options every ScreenCast call takes, minus the caller's own keys.
    ///
    /// `CreateSession` additionally wants the handle of the session it is about
    /// to create; it is the only call that reads `session_handle_token`.
    fn tokens(
        &mut self,
        token: &str,
        with_session_token: bool,
    ) -> HashMap<String, Variant<'static>> {
        let mut options: HashMap<String, Variant<'static>> = HashMap::new();
        options.insert("handle_token".to_string(), Variant::from(token.to_string()));
        if with_session_token {
            let session_token = self.next_token("snow_shot_session");
            options.insert(
                "session_handle_token".to_string(),
                Variant::from(session_token),
            );
        }
        options
    }

    /// Make the call and wait for the `Response` that answers it.
    ///
    /// `body` is the complete argument list, options dictionary included.
    fn finish<B>(
        &mut self,
        step: Step,
        member: &str,
        predicted: String,
        body: B,
    ) -> Result<PortalResponse, PortalError>
    where
        B: Serialize + Type,
    {
        let connection = &self.connection;
        let timeout = self.timeout;
        zbus::block_on(async {
            let mut stream = subscribe(connection, &predicted)
                .await
                .map_err(|error| transport_error(step, error))?;

            let reply = connection
                .call_method(
                    Some(DESKTOP_SERVICE),
                    DESKTOP_PATH,
                    Some(SCREENCAST_INTERFACE),
                    member,
                    &body,
                )
                .await
                .map_err(|error| transport_error(step, error))?;

            let reply_body = reply.body();
            let returned: ObjectPath<'_> = reply_body
                .deserialize()
                .map_err(|error| transport_error(step, error))?;

            // Only re-subscribe when the portal disagreed, so the common path
            // keeps the subscription that was already waiting before the call.
            let mut moved = if returned.as_str() == predicted {
                None
            } else {
                Some(
                    subscribe(connection, returned.as_str())
                        .await
                        .map_err(|error| transport_error(step, error))?,
                )
            };

            match moved.as_mut() {
                Some(stream) => await_response(stream, timeout).await,
                None => await_response(&mut stream, timeout).await,
            }
            .map_err(|error| attach_step(error, step))
        })
    }
}

impl PortalTransport for BusTransport {
    fn screencast_version(&mut self) -> Result<u32, PortalError> {
        let reply = zbus::block_on(self.connection.call_method(
            Some(DESKTOP_SERVICE),
            DESKTOP_PATH,
            Some("org.freedesktop.DBus.Properties"),
            "Get",
            &(SCREENCAST_INTERFACE, "version"),
        ))
        .map_err(|error| PortalError::Unavailable(error.to_string()))?;

        let body = reply.body();
        let value: Variant<'_> = body
            .deserialize()
            .map_err(|error| PortalError::Unavailable(error.to_string()))?;
        match value {
            Variant::U32(version) => Ok(version),
            other => Err(PortalError::Unavailable(format!(
                "the ScreenCast version property is not a u32 but {}",
                other.value_signature()
            ))),
        }
    }

    fn create_session(
        &mut self,
        _options: &[(&'static str, PortalValue)],
    ) -> Result<PortalResponse, PortalError> {
        // The only options `CreateSession` takes are the two tokens, which this
        // layer owns; the caller's list is empty by contract, see `negotiate`.
        let step = Step::CreateSession;
        let (token, predicted) = self.begin(step)?;
        let options = self.tokens(&token, true);
        self.finish(step, "CreateSession", predicted, options)
    }

    fn select_sources(
        &mut self,
        session: &str,
        select: &[(&'static str, PortalValue)],
    ) -> Result<PortalResponse, PortalError> {
        let step = Step::SelectSources;
        let session = session_path(session, step)?;
        let (token, predicted) = self.begin(step)?;
        let mut options = self.tokens(&token, false);
        options.extend(select_dictionary(select));
        self.finish(step, "SelectSources", predicted, (session, options))
    }

    fn start(
        &mut self,
        session: &str,
        parent_window: &str,
        _options: &[(&'static str, PortalValue)],
    ) -> Result<PortalResponse, PortalError> {
        let step = Step::Start;
        let session = session_path(session, step)?;
        let (token, predicted) = self.begin(step)?;
        let options = self.tokens(&token, false);
        self.finish(
            step,
            "Start",
            predicted,
            (session, parent_window.to_string(), options),
        )
    }
}

/// Turn a handle the portal gave us back into an object path argument.
fn session_path(session: &str, step: Step) -> Result<ObjectPath<'static>, PortalError> {
    ObjectPath::try_from(session.to_string()).map_err(|error| PortalError::Failed {
        step,
        detail: format!("the portal returned an unusable session handle: {error}"),
    })
}

/// The `a{sv}` a caller's option list turns into.
fn select_dictionary(options: &[(&'static str, PortalValue)]) -> HashMap<String, Variant<'static>> {
    options
        .iter()
        .filter_map(|(key, value)| to_variant(value).map(|value| ((*key).to_string(), value)))
        .collect()
}

/// Model a [`PortalValue`] as the D-Bus value the portal expects.
///
/// Only the shapes the caller can construct are handled; a value with no
/// representation here is dropped, and the caller's option list is built by
/// this crate, so that cannot happen silently in practice.
fn to_variant(value: &PortalValue) -> Option<Variant<'static>> {
    match value {
        PortalValue::U32(inner) => Some(Variant::from(*inner)),
        PortalValue::I32(inner) => Some(Variant::from(*inner)),
        PortalValue::U64(inner) => Some(Variant::from(*inner)),
        PortalValue::Bool(inner) => Some(Variant::from(*inner)),
        PortalValue::Str(inner) => Some(Variant::from(inner.clone())),
        PortalValue::ObjectPath(inner) => Some(Variant::from(inner.clone())),
        PortalValue::Array(_)
        | PortalValue::Struct(_)
        | PortalValue::Dict(_)
        | PortalValue::Opaque => None,
    }
}

/// Turn a reply value into the model the negotiation works on.
fn to_portal_value(value: &Variant<'_>) -> PortalValue {
    match value {
        Variant::U32(inner) => PortalValue::U32(*inner),
        Variant::I32(inner) => PortalValue::I32(*inner),
        Variant::U64(inner) => PortalValue::U64(*inner),
        Variant::Bool(inner) => PortalValue::Bool(*inner),
        Variant::Str(inner) => PortalValue::Str(inner.to_string()),
        Variant::ObjectPath(inner) => PortalValue::ObjectPath(inner.to_string()),
        // A variant is transparent: the portal wraps everything in `v` exactly
        // once, and the wrapped value is what the caller means.
        Variant::Variant(inner) => to_portal_value(inner),
        Variant::Array(array) => PortalValue::Array(array.iter().map(to_portal_value).collect()),
        Variant::Dict(dict) => PortalValue::Dict(
            dict.iter()
                .filter_map(|(key, value)| {
                    // `a{sv}` keys are strings; anything else cannot be looked
                    // up by name and is dropped rather than guessed at.
                    let key = match key {
                        Variant::Str(key) => key.to_string(),
                        Variant::ObjectPath(key) => key.to_string(),
                        _ => return None,
                    };
                    Some((key, to_portal_value(value)))
                })
                .collect(),
        ),
        Variant::Structure(structure) => {
            PortalValue::Struct(structure.fields().iter().map(to_portal_value).collect())
        }
        // Bytes, file descriptors, signatures and the 16-bit and float types do
        // not appear in any ScreenCast reply; modelling them would be inventing
        // a use for them.
        _ => PortalValue::Opaque,
    }
}

/// Subscribe to `Response` on one request path.
async fn subscribe(connection: &Connection, path: &str) -> zbus::Result<MessageStream> {
    let rule = MatchRule::builder()
        .msg_type(message::Type::Signal)
        .interface(REQUEST_INTERFACE)
        .member("Response")
        .path(path)
        .build()?;
    MessageStream::for_match_rule(
        rule,
        connection,
        Some(
            // One answer per request, plus slack: a queue of one would drop the
            // signal if the task were not polled in time.
            std::num::NonZeroUsize::new(4).expect("4 is not zero"),
        ),
    )
    .await
}

/// The next message on a stream.
async fn next_message(stream: &mut MessageStream) -> Option<zbus::Result<Message>> {
    std::future::poll_fn(|context| Pin::new(&mut *stream).poll_next(context)).await
}

/// Wait for the first `Response` on a subscribed path.
async fn await_response(
    stream: &mut MessageStream,
    timeout: Duration,
) -> Result<PortalResponse, PortalError> {
    let message = timeout_after(timeout, next_message(stream))
        .await
        .map_err(|_| PortalError::Failed {
            step: Step::Start,
            detail: format!("the portal sent no Response within {timeout:?}"),
        })?
        .ok_or_else(|| PortalError::Failed {
            step: Step::Start,
            detail: "the portal closed the connection before answering".to_string(),
        })?
        .map_err(|error| PortalError::Failed {
            step: Step::Start,
            detail: error.to_string(),
        })?;

    let body = message.body();
    let (code, results): (u32, HashMap<String, Variant<'_>>) =
        body.deserialize().map_err(|error| PortalError::Failed {
            step: Step::Start,
            detail: format!("unusable Response body: {error}"),
        })?;

    Ok(PortalResponse {
        code: super::ResponseCode::from_raw(code),
        results: PortalValue::Dict(
            results
                .iter()
                .map(|(key, value)| (key.clone(), to_portal_value(value)))
                .collect(),
        ),
    })
}

/// A transport failure, with the step left blank for the caller to fill in.
fn transport_error(step: Step, error: zbus::Error) -> PortalError {
    PortalError::Failed {
        step,
        detail: error.to_string(),
    }
}

/// Replace the placeholder step that [`await_response`] and [`transport_error`]
/// cannot know with the one the call belongs to.
fn attach_step(error: PortalError, step: Step) -> PortalError {
    match error {
        PortalError::Failed { detail, .. } => PortalError::Failed { step, detail },
        other => other,
    }
}

/// Read the interface version without showing any dialog.
///
/// `None` means "there is no usable ScreenCast interface here", which is a
/// normal state on X11 sessions, in containers, and on machines with no portal
/// installed -- not a fault to report.
pub fn screencast_version(timeout: Duration) -> Option<u32> {
    let mut transport = BusTransport::connect().ok()?;
    zbus::block_on(timeout_after(timeout, async {
        transport.screencast_version()
    }))
    .ok()
    .and_then(Result::ok)
}

/// A negotiated session, with the bus connection that keeps it alive.
///
/// Dropping this closes the portal session: a session exists only for the client
/// that created it, and a hold on the bus connection is the cheap way to say
/// "still mine".
pub struct PortalSession {
    #[allow(dead_code, reason = "keeps the portal session open; see the type docs")]
    transport: BusTransport,
    session: ScreenCastSession,
}

impl PortalSession {
    pub fn streams(&self) -> &[super::ScreenCastStream] {
        &self.session.streams
    }

    pub fn restore_token(&self) -> Option<&str> {
        self.session.restore_token.as_deref()
    }

    pub fn version(&self) -> u32 {
        self.session.version
    }
}

/// Negotiate a monitor capture session, showing the portal's dialog.
///
/// Blocks: the portal asks the user for permission, and the answer arrives as a
/// signal. Callers on the UI thread should run this off it.
pub fn negotiate_screencast(options: &NegotiateOptions) -> Result<PortalSession, PortalError> {
    let mut transport = BusTransport::connect()?;
    let session = super::negotiate(&mut transport, options)?;
    Ok(PortalSession { transport, session })
}
