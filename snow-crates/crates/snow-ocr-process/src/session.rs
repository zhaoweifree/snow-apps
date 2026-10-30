//! A session owns no submitted image. Preparation releases the old engine
//! before loading; recreation consumes its request and may share immutable
//! models. Either failure leaves the session empty.
#[derive(Default)]
pub struct Session<E> {
    engine: Option<E>,
}

impl<E> Session<E> {
    pub fn empty() -> Self {
        Self { engine: None }
    }

    pub fn release(&mut self) {
        self.engine = None;
    }

    pub fn prepare(&mut self, create: impl FnOnce() -> Option<E>) -> bool {
        self.release();
        self.engine = create();
        self.engine.is_some()
    }

    /// Consume the old request so its mutable storage can be dropped while
    /// sharing only immutable model resources with the replacement.
    pub fn recreate(&mut self, create: impl FnOnce(E) -> Option<E>) -> bool {
        self.engine = self.engine.take().and_then(create);
        self.engine.is_some()
    }

    pub fn engine(&mut self) -> &mut Option<E> {
        &mut self.engine
    }
}

#[cfg(test)]
mod tests {
    use super::Session;
    use std::{cell::RefCell, rc::Rc};

    struct Engine(Rc<RefCell<Vec<&'static str>>>);
    impl Drop for Engine {
        fn drop(&mut self) {
            self.0.borrow_mut().push("drop");
        }
    }

    #[test]
    fn preparation_is_image_free_and_replacement_drops_before_loading() {
        let events = Rc::new(RefCell::new(Vec::new()));
        let mut session = Session::empty();
        assert!(session.engine().is_none());
        let create = || {
            events.borrow_mut().push("load");
            Some(Engine(Rc::clone(&events)))
        };
        assert!(session.prepare(create));
        assert_eq!(*events.borrow(), ["load"]);
        // Using an engine does not change the ownership rule for the next idle cycle.
        assert!(session.engine().is_some());
        assert!(session.prepare(create));
        assert_eq!(*events.borrow(), ["load", "drop", "load"]);
        session.release();
        session.release();
        assert_eq!(*events.borrow(), ["load", "drop", "load", "drop"]);
    }

    #[test]
    fn failed_preparation_cannot_retain_the_previous_engine() {
        let mut session = Session::empty();
        assert!(session.prepare(|| Some(42)));
        assert!(!session.prepare(|| None));
        assert!(session.engine().is_none());
        assert!(session.prepare(|| Some(43)));
        assert_eq!(session.engine().as_ref(), Some(&43));
    }

    #[test]
    fn recreation_consumes_the_old_request_and_failure_leaves_no_engine() {
        let events = Rc::new(RefCell::new(Vec::new()));
        let mut session = Session::empty();
        assert!(!session.recreate(|_: Engine| panic!("empty session cannot be recreated")));
        assert!(session.prepare(|| Some(Engine(Rc::clone(&events)))));
        assert!(session.recreate(|previous| {
            drop(previous);
            events.borrow_mut().push("recreate");
            Some(Engine(Rc::clone(&events)))
        }));
        assert_eq!(*events.borrow(), ["drop", "recreate"]);
        assert!(!session.recreate(|previous| {
            drop(previous);
            None
        }));
        assert!(session.engine().is_none());
        session.release();
        assert_eq!(*events.borrow(), ["drop", "recreate", "drop"]);
    }
}
