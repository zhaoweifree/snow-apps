//! Request-scoped raster allocation admission, inherited explicitly by native workers.
//!
//! No policy is installed for ordinary capture. Hooks may execute on a native worker
//! and must be thread safe. The installing caller owns their lifetime; every worker
//! inheriting a policy must be joined before that caller releases borrowed context.
use std::cell::RefCell;
use std::sync::Arc;

pub type RasterAdmission = Arc<dyn Fn(u32, u32) -> bool + Send + Sync>;
thread_local! {
    static CURRENT: RefCell<Option<RasterAdmission>> = const { RefCell::new(None) };
}

pub fn current() -> Option<RasterAdmission> {
    CURRENT.with(|current| current.borrow().clone())
}

/// Install a policy until the operation returns, including during unwinding.
pub fn with_policy<T>(policy: Option<RasterAdmission>, operation: impl FnOnce() -> T) -> T {
    struct Restore(Option<RasterAdmission>);
    impl Drop for Restore {
        fn drop(&mut self) {
            CURRENT.with(|current| *current.borrow_mut() = self.0.take());
        }
    }
    let _restore = Restore(CURRENT.with(|current| current.replace(policy)));
    operation()
}

/// Check the actual shape before creating or resizing a native or CPU raster.
/// Clone the hook before invoking it so reentrant scopes never borrow the TLS cell.
pub fn allows(width: u32, height: u32) -> bool {
    current().is_none_or(|admit| admit(width, height))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn scopes_restore_and_propagate_explicitly_without_affecting_other_threads() {
        assert!(allows(u32::MAX, u32::MAX));
        with_policy(Some(Arc::new(|w, h| w <= 4 && h <= 3)), || {
            assert!(allows(4, 3));
            assert!(!allows(3, 4)); // Same area is not the same shape.
            with_policy(Some(Arc::new(|_, _| false)), || assert!(!allows(1, 1)));
            assert!(allows(4, 3));
            let inherited = current();
            std::thread::spawn(move || {
                assert!(allows(100, 100));
                with_policy(inherited, || assert!(!allows(5, 3)));
                assert!(allows(100, 100));
            })
            .join()
            .unwrap();
            let result = std::panic::catch_unwind(|| {
                with_policy(None, || panic!("restore policy on unwind"));
            });
            assert!(result.is_err());
            assert!(!allows(5, 3));
        });
        assert!(allows(u32::MAX, u32::MAX));
    }
}
