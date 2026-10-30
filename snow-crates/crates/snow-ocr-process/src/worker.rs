use super::*;

pub(super) enum Work {
    Prepare(u64, Config),
    Release(u64),
    Recognize(Job),
}
pub(super) enum WorkResult {
    Prepared(u64, bool),
    Released(u64),
    Complete(Box<Completion>),
}

fn infer(job: Job, engine: &mut Option<Engine>) -> Completion {
    let started = Instant::now();
    let queue_ms = job.queued_at.elapsed().as_millis();
    diagnostics::operation_started(job.id);
    let result = run_unless_cancelled(&job.cancelled, || {
        engine
            .as_mut()
            .ok_or_else(|| "OCR session is not prepared".to_string())?
            .run(&job.input)
            .map_err(|error| error.to_string())
    });
    let cancelled = job.cancelled.load(Ordering::Acquire);
    eprintln!(
        "{}",
        serde_json::json!({
            "event": "ocr.worker_finished", "fields": {
                "operation": job.id.to_string(), "queue_ms": queue_ms,
                "initialization_ms": 0, "inference_ms": started.elapsed().as_millis(),
                "worker_ms": started.elapsed().as_millis(), "backend": "simd-paddle-ocr-rs",
                "outcome": if cancelled { "cancelled" } else if result.is_ok() { "succeeded" } else { "failed" }
            }
        })
    );
    Completion {
        id: job.id,
        result,
        cancelled,
    }
}

pub(super) fn worker_loop(rx: mpsc::Receiver<Work>, tx: mpsc::Sender<Event>) {
    let mut session = crate::session::Session::empty();
    let mut configuration = None;
    let thread_budget = num_cpus::get_physical().max(1);
    for work in rx {
        let result = match work {
            Work::Prepare(id, next) => {
                let ready = if configuration.as_ref() == Some(&next) {
                    session.recreate(|previous| {
                        initialize_engine(&next, thread_budget, Some(previous))
                    })
                } else {
                    session.prepare(|| initialize_engine(&next, thread_budget, None))
                };
                configuration = ready.then_some(next);
                WorkResult::Prepared(id, ready)
            }
            Work::Release(id) => {
                session.release();
                configuration = None;
                WorkResult::Released(id)
            }
            Work::Recognize(job) => {
                let completion = infer(job, session.engine());
                WorkResult::Complete(Box::new(completion))
            }
        };
        if tx.send(Event::Completed(result)).is_err() {
            break;
        }
    }
}
