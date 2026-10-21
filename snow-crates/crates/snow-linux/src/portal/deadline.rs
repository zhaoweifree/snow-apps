//! A dead-line for a future that a portal may simply never resolve.
//!
//! The portal answers a request by emitting a `Response` signal. A portal that
//! crashed between accepting the method call and showing the dialog never emits
//! one, and a plain `block_on` on that signal would hang the capture call with
//! no way out: the caller is a synchronous `capture()` and has nothing to
//! cancel with. So the wait is bounded here.
//!
//! `zbus` has a timeout of its own, but only on the runtime it owns and only
//! inside the crate, so the combinator is written instead of borrowed. It is
//! runtime-agnostic on purpose: it holds no timer thread per poll and needs no
//! timer wheel, just one parked thread that wakes the polling task once.

use std::future::Future;
use std::pin::{Pin, pin};
use std::sync::{Arc, Mutex, PoisonError};
use std::task::{Context, Poll, Waker};
use std::time::Duration;

/// Returned when the future did not resolve in time.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Elapsed;

impl std::fmt::Display for Elapsed {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str("the operation did not finish in time")
    }
}

struct Shared {
    fired: bool,
    waker: Option<Waker>,
}

/// Resolves once the duration has passed, whichever executor polls it.
struct Deadline {
    shared: Arc<Mutex<Shared>>,
}

impl Deadline {
    fn after(duration: Duration) -> Self {
        let shared = Arc::new(Mutex::new(Shared {
            fired: false,
            waker: None,
        }));
        let thread_side = Arc::clone(&shared);
        // The thread outlives an early return by at most `duration`. That is
        // affordable here because the only duration in use is measured in
        // seconds and the call happens once per capture session, not per frame.
        std::thread::spawn(move || {
            std::thread::sleep(duration);
            let waker = {
                let mut state = thread_side.lock().unwrap_or_else(PoisonError::into_inner);
                state.fired = true;
                state.waker.take()
            };
            if let Some(waker) = waker {
                waker.wake();
            }
        });
        Self { shared }
    }
}

impl Future for Deadline {
    type Output = ();

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<()> {
        let mut state = self.shared.lock().unwrap_or_else(PoisonError::into_inner);
        if state.fired {
            return Poll::Ready(());
        }
        // Replacing unconditionally is correct: the executor is entitled to
        // poll with a different waker every time.
        state.waker = Some(context.waker().clone());
        Poll::Pending
    }
}

/// Await `future`, or give up after `duration`.
///
/// The future is polled before the timer, so an answer that arrived by the time
/// the deadline passed still wins.
pub async fn timeout_after<F: Future>(duration: Duration, future: F) -> Result<F::Output, Elapsed> {
    let mut future = pin!(future);
    let mut deadline = pin!(Deadline::after(duration));

    std::future::poll_fn(|context| {
        if let Poll::Ready(output) = future.as_mut().poll(context) {
            return Poll::Ready(Ok(output));
        }
        deadline.as_mut().poll(context).map(|()| Err(Elapsed))
    })
    .await
}

#[cfg(test)]
mod tests {
    use super::{Elapsed, timeout_after};
    use std::future::Future;
    use std::sync::{Arc, Mutex};
    use std::task::{Context, Poll, Wake, Waker};
    use std::time::{Duration, Instant};

    struct Unpark(std::thread::Thread);

    impl Wake for Unpark {
        fn wake(self: Arc<Self>) {
            self.0.unpark();
        }
    }

    /// Drive one future to completion on this thread.
    ///
    /// The crate's real wait runs on `zbus`'s runtime; these tests only need
    /// something that parks and wakes correctly, and using a real runtime here
    /// would test that runtime instead of the combinator.
    fn block_on<F: Future>(future: F) -> F::Output {
        let waker = Waker::from(Arc::new(Unpark(std::thread::current())));
        let mut context = Context::from_waker(&waker);
        let mut future = std::pin::pin!(future);
        loop {
            if let Poll::Ready(output) = future.as_mut().poll(&mut context) {
                return output;
            }
            std::thread::park();
        }
    }

    /// Resolves after `duration` on its own thread, so the combinator is the
    /// only thing being measured.
    fn sleep(duration: Duration) -> impl Future<Output = ()> {
        struct Shared {
            done: bool,
            waker: Option<Waker>,
        }

        let shared = Arc::new(Mutex::new(Shared {
            done: false,
            waker: None,
        }));
        let thread_side = Arc::clone(&shared);
        std::thread::spawn(move || {
            std::thread::sleep(duration);
            let waker = {
                let mut state = thread_side.lock().unwrap();
                state.done = true;
                state.waker.take()
            };
            if let Some(waker) = waker {
                waker.wake();
            }
        });
        std::future::poll_fn(move |context| {
            let mut state = shared.lock().unwrap();
            if state.done {
                return Poll::Ready(());
            }
            state.waker = Some(context.waker().clone());
            Poll::Pending
        })
    }

    #[test]
    fn a_ready_future_keeps_its_answer() {
        let answer = block_on(timeout_after(Duration::from_secs(5), async { 7 }));
        assert_eq!(answer, Ok(7));
    }

    #[test]
    fn a_future_that_never_resolves_times_out() {
        let started = Instant::now();
        let answer = block_on(timeout_after(
            Duration::from_millis(50),
            std::future::pending::<u8>(),
        ));
        assert_eq!(answer, Err(Elapsed));
        // Not a strict upper bound -- a loaded machine may overshoot -- but it
        // must not have returned before the deadline.
        assert!(started.elapsed() >= Duration::from_millis(50));
    }

    #[test]
    fn an_answer_that_beats_the_deadline_is_returned() {
        let answer = block_on(timeout_after(
            Duration::from_secs(5),
            sleep(Duration::from_millis(10)),
        ));
        assert_eq!(answer, Ok(()));
    }

    #[test]
    fn an_answer_that_misses_the_deadline_is_dropped() {
        let answer = block_on(timeout_after(
            Duration::from_millis(10),
            sleep(Duration::from_millis(200)),
        ));
        assert_eq!(answer, Err(Elapsed));
    }
}
