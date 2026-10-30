use super::*;
use std::ffi::CStr;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;
use std::sync::mpsc::{Receiver, SyncSender, TryRecvError, sync_channel};
use std::thread::{self, JoinHandle};

#[derive(Clone, Copy, Default)]
enum PngCompression {
    #[default]
    Fast,
    Balanced,
    Best,
}

#[derive(Clone, Copy)]
enum PngExportStage {
    Preparing,
    Encoding,
    Committing,
}

#[derive(Clone, Copy)]
struct PngExportProgress {
    stage: PngExportStage,
    rows_written: u32,
    total_rows: u32,
    percent: f32,
}

struct PngExportRequest {
    output_path: PathBuf,
    compression: PngCompression,
    overwrite: bool,
}

#[derive(Debug)]
enum PngExportError {
    Canceled,
    Failed(String),
}

impl std::fmt::Display for PngExportError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Canceled => formatter.write_str("PNG export canceled"),
            Self::Failed(message) => formatter.write_str(message),
        }
    }
}

struct PngExportTask {
    cancel: Arc<AtomicBool>,
    progress: Receiver<PngExportProgress>,
    join: Option<JoinHandle<Result<(), PngExportError>>>,
}

impl PngExportTask {
    fn spawn(
        snapshot: StitchImageSnapshot,
        request: PngExportRequest,
    ) -> Result<Self, StitchError> {
        if request.output_path.as_os_str().is_empty() {
            return Err(StitchError::InvalidOptions {
                message: "PNG output path must not be empty".to_owned(),
            });
        }
        if request.output_path.exists() && !request.overwrite {
            return Err(StitchError::InvalidFrame {
                message: format!(
                    "PNG output already exists: {}",
                    request.output_path.display()
                ),
            });
        }
        let parent = request
            .output_path
            .parent()
            .unwrap_or_else(|| Path::new("."));
        if !parent.is_dir() {
            return Err(StitchError::InvalidFrame {
                message: format!("PNG output directory does not exist: {}", parent.display()),
            });
        }
        let cancel = Arc::new(AtomicBool::new(false));
        let worker_cancel = Arc::clone(&cancel);
        let (progress_tx, progress) = sync_channel(16);
        let join = thread::Builder::new()
            .name("snow-stitch-png-export".to_owned())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                export_png(snapshot, request, worker_cancel, progress_tx)
            })
            .map_err(|error| StitchError::InvalidFrame {
                message: format!("failed to spawn PNG export worker: {error}"),
            })?;
        Ok(Self {
            cancel,
            progress,
            join: Some(join),
        })
    }

    fn cancel(&self) {
        self.cancel.store(true, Ordering::Release);
    }

    fn try_progress(&self) -> Result<PngExportProgress, TryRecvError> {
        self.progress.try_recv()
    }

    fn is_finished(&self) -> bool {
        self.join.as_ref().is_some_and(JoinHandle::is_finished)
    }

    fn wait(mut self) -> Result<(), PngExportError> {
        self.join
            .take()
            .expect("live export task has a worker")
            .join()
            .map_err(|_| PngExportError::Failed("PNG export worker panicked".to_owned()))?
    }
}

impl Drop for PngExportTask {
    fn drop(&mut self) {
        self.cancel();
        if let Some(join) = self.join.take() {
            let _ = join.join();
        }
    }
}

impl StitchImageSnapshot {
    fn export_png(&self, request: PngExportRequest) -> Result<PngExportTask, StitchError> {
        PngExportTask::spawn(self.clone(), request)
    }
}

fn export_png(
    snapshot: StitchImageSnapshot,
    request: PngExportRequest,
    cancel: Arc<AtomicBool>,
    progress: SyncSender<PngExportProgress>,
) -> Result<(), PngExportError> {
    let emit = |stage, rows_written| {
        let total_rows = snapshot.height();
        let percent = rows_written as f32 * 100.0 / total_rows as f32;
        let _ = progress.try_send(PngExportProgress {
            stage,
            rows_written,
            total_rows,
            percent,
        });
    };
    if cancel.load(Ordering::Acquire) {
        return Err(PngExportError::Canceled);
    }
    emit(PngExportStage::Preparing, 0);

    let parent = request
        .output_path
        .parent()
        .unwrap_or_else(|| Path::new("."));
    let mut temporary = tempfile::Builder::new()
        .prefix(".snow-shot-")
        .suffix(".png.tmp")
        .tempfile_in(parent)
        .map_err(|error| PngExportError::Failed(error.to_string()))?;
    {
        let mut encoder =
            png::Encoder::new(temporary.as_file_mut(), snapshot.width(), snapshot.height());
        encoder.set_color(png::ColorType::Rgba);
        encoder.set_depth(png::BitDepth::Eight);
        encoder.set_filter(png::Filter::Adaptive);
        encoder.set_compression(match request.compression {
            PngCompression::Fast => png::Compression::Fast,
            PngCompression::Balanced => png::Compression::Balanced,
            PngCompression::Best => png::Compression::High,
        });
        let mut writer = encoder
            .write_header()
            .map_err(|error| PngExportError::Failed(error.to_string()))?;
        {
            let mut stream = writer
                .stream_writer_with_size(64 * 1024)
                .map_err(|error| PngExportError::Failed(error.to_string()))?;
            let row_bytes = (snapshot.width() as usize)
                .checked_mul(4)
                .ok_or_else(|| PngExportError::Failed("PNG row length overflow".to_owned()))?;
            let strip_rows = snapshot.height().min(64);
            let strip_bytes = row_bytes
                .checked_mul(strip_rows as usize)
                .ok_or_else(|| PngExportError::Failed("PNG strip length overflow".to_owned()))?;
            let mut strip = RasterBuffer::zeroed(strip_bytes);
            let mut rows_written = 0;
            while rows_written < snapshot.height() {
                if cancel.load(Ordering::Acquire) {
                    return Err(PngExportError::Canceled);
                }
                let rows = strip_rows.min(snapshot.height() - rows_written);
                let chunk = &mut strip[..row_bytes * rows as usize];
                snapshot
                    .canvas
                    .copy_rows(rows_written, rows, chunk)
                    .map_err(|error| PngExportError::Failed(error.to_string()))?;
                stream
                    .write_all(chunk)
                    .map_err(|error| PngExportError::Failed(error.to_string()))?;
                rows_written += rows;
                emit(PngExportStage::Encoding, rows_written);
            }
            stream
                .finish()
                .map_err(|error| PngExportError::Failed(error.to_string()))?;
        }
        writer
            .finish()
            .map_err(|error| PngExportError::Failed(error.to_string()))?;
    }
    temporary
        .as_file_mut()
        .flush()
        .map_err(|error| PngExportError::Failed(error.to_string()))?;
    temporary
        .as_file()
        .sync_all()
        .map_err(|error| PngExportError::Failed(error.to_string()))?;
    if cancel.load(Ordering::Acquire) {
        return Err(PngExportError::Canceled);
    }
    emit(PngExportStage::Committing, snapshot.height());
    if request.overwrite {
        temporary.persist(&request.output_path)
    } else {
        temporary.persist_noclobber(&request.output_path)
    }
    .map_err(|error| PngExportError::Failed(error.error.to_string()))?;
    Ok(())
}

pub struct SnowStitchExportTaskImpl {
    task: Option<PngExportTask>,
    status: SnowStitchExportStatus,
    error: CString,
}

#[unsafe(no_mangle)]
/// # Safety
/// `snapshot` and `config` must remain live during the call. The path pointer
/// in `config` must reference a valid NUL-terminated UTF-8 string.
pub unsafe extern "C" fn snow_stitch_snapshot_export_png(
    snapshot: *const SnowStitchSnapshotImpl,
    config: *const SnowStitchPngExportConfig,
) -> *mut SnowStitchExportTaskImpl {
    let Some(snapshot) = (unsafe { snapshot.as_ref() }) else {
        set_last_error("stitch snapshot is null");
        return ptr::null_mut();
    };
    let Some(config) = (unsafe { config.as_ref() }) else {
        set_last_error("PNG export config is null");
        return ptr::null_mut();
    };
    if config.output_path_utf8.is_null() {
        set_last_error("PNG output path is null");
        return ptr::null_mut();
    }
    let path = match unsafe { CStr::from_ptr(config.output_path_utf8) }.to_str() {
        Ok(path) => PathBuf::from(path),
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let request = PngExportRequest {
        output_path: path,
        compression: match config.compression {
            SnowStitchPngCompression::Fast => PngCompression::Fast,
            SnowStitchPngCompression::Balanced => PngCompression::Balanced,
            SnowStitchPngCompression::Best => PngCompression::Best,
        },
        overwrite: config.overwrite != 0,
    };
    match snapshot.snapshot.export_png(request) {
        Ok(task) => {
            clear_last_error();
            Box::into_raw(Box::new(SnowStitchExportTaskImpl {
                task: Some(task),
                status: SnowStitchExportStatus::Running,
                error: CString::new("").expect("empty C string"),
            }))
        }
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}

fn ffi_progress(progress: PngExportProgress) -> SnowStitchExportProgress {
    SnowStitchExportProgress {
        stage: match progress.stage {
            PngExportStage::Preparing => SnowStitchExportStage::Preparing,
            PngExportStage::Encoding => SnowStitchExportStage::Encoding,
            PngExportStage::Committing => SnowStitchExportStage::Committing,
        },
        rows_written: progress.rows_written,
        total_rows: progress.total_rows,
        percent: progress.percent,
    }
}

fn finish_export(task: &mut SnowStitchExportTaskImpl, block: bool) {
    if task.status != SnowStitchExportStatus::Running {
        return;
    }
    let should_finish = block || task.task.as_ref().is_some_and(PngExportTask::is_finished);
    if !should_finish {
        return;
    }
    let Some(worker) = task.task.take() else {
        return;
    };
    match worker.wait() {
        Ok(_) => task.status = SnowStitchExportStatus::Complete,
        Err(PngExportError::Canceled) => {
            task.status = SnowStitchExportStatus::Canceled;
            task.error = sanitize_cstring("PNG export canceled");
        }
        Err(error) => {
            task.status = SnowStitchExportStatus::Failed;
            task.error = sanitize_cstring(error.to_string());
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `task` must be null or point to a live export task.
pub unsafe extern "C" fn snow_stitch_export_task_cancel(task: *mut SnowStitchExportTaskImpl) {
    if let Some(task) = unsafe { task.as_ref() }
        && let Some(worker) = &task.task
    {
        worker.cancel();
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `task` must be live and exclusively accessed. Non-null output pointers must
/// point to writable values of their corresponding C types.
pub unsafe extern "C" fn snow_stitch_export_task_poll(
    task: *mut SnowStitchExportTaskImpl,
    out_progress: *mut SnowStitchExportProgress,
    out_has_progress: *mut u8,
) -> SnowStitchExportStatus {
    let Some(task) = (unsafe { task.as_mut() }) else {
        set_last_error("PNG export task is null");
        return SnowStitchExportStatus::Failed;
    };
    if !out_has_progress.is_null() {
        unsafe { *out_has_progress = 0 };
    }
    if let Some(worker) = &task.task
        && let Ok(progress) = worker.try_progress()
    {
        if !out_progress.is_null() {
            unsafe { *out_progress = ffi_progress(progress) };
        }
        if !out_has_progress.is_null() {
            unsafe { *out_has_progress = 1 };
        }
    }
    finish_export(task, false);
    task.status
}

#[unsafe(no_mangle)]
/// # Safety
/// `task` must point to a live, exclusively accessed export task.
pub unsafe extern "C" fn snow_stitch_export_task_wait(
    task: *mut SnowStitchExportTaskImpl,
) -> SnowStitchExportStatus {
    let Some(task) = (unsafe { task.as_mut() }) else {
        set_last_error("PNG export task is null");
        return SnowStitchExportStatus::Failed;
    };
    finish_export(task, true);
    task.status
}

#[unsafe(no_mangle)]
/// # Safety
/// `task` must remain live while the returned message pointer is read.
pub unsafe extern "C" fn snow_stitch_export_task_error_message(
    task: *const SnowStitchExportTaskImpl,
) -> *const c_char {
    unsafe { task.as_ref() }.map_or(ptr::null(), |task| task.error.as_ptr())
}

#[unsafe(no_mangle)]
/// # Safety
/// `task` must be null or a live export task that has not already been
/// destroyed.
pub unsafe extern "C" fn snow_stitch_export_task_destroy(task: *mut SnowStitchExportTaskImpl) {
    if !task.is_null() {
        drop(unsafe { Box::from_raw(task) });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn streamed_png_matches_unaligned_vertical_and_horizontal_snapshots() {
        use snow_stitch_images::CANVAS_TILE_SPAN;

        let temporary = tempfile::tempdir().unwrap();
        for axis in [StitchAxis::Vertical, StitchAxis::Horizontal] {
            let (width, height) = match axis {
                StitchAxis::Vertical => (7, CANVAS_TILE_SPAN * 2 + 73),
                StitchAxis::Horizontal => (CANVAS_TILE_SPAN * 2 + 73, 137),
            };
            let source: Vec<_> = (0..width as usize * height as usize * 4)
                .map(|index| ((index * 37 + index / 17) % 256) as u8)
                .collect();
            let frame = Frame::new(width, height, PixelFormat::Rgba8, source.clone()).unwrap();
            let full = TiledCanvasSnapshot::from_frame_for_axis(frame, axis);
            let extent = axis.primary_extent(width, height);
            let canvas = full.slice_axis(13, extent - 11).unwrap();
            drop(full);
            let snapshot = StitchImageSnapshot { canvas };
            let expected = match axis {
                StitchAxis::Vertical => source
                    [13 * width as usize * 4..(height - 11) as usize * width as usize * 4]
                    .to_vec(),
                StitchAxis::Horizontal => source
                    .chunks_exact(width as usize * 4)
                    .flat_map(|row| row[13 * 4..(width - 11) as usize * 4].iter().copied())
                    .collect(),
            };
            for (index, compression) in [
                PngCompression::Fast,
                PngCompression::Balanced,
                PngCompression::Best,
            ]
            .into_iter()
            .enumerate()
            {
                let output = temporary.path().join(format!("{axis:?}-{index}.png"));
                let (progress_tx, progress) = sync_channel(64);
                export_png(
                    snapshot.clone(),
                    PngExportRequest {
                        output_path: output.clone(),
                        compression,
                        overwrite: false,
                    },
                    Arc::new(AtomicBool::new(false)),
                    progress_tx,
                )
                .unwrap();
                let mut reader = png::Decoder::new(std::io::BufReader::new(
                    std::fs::File::open(&output).unwrap(),
                ))
                .read_info()
                .unwrap();
                let mut decoded = vec![0; reader.output_buffer_size().unwrap()];
                let info = reader.next_frame(&mut decoded).unwrap();
                assert_eq!(
                    (info.width, info.height),
                    (snapshot.width(), snapshot.height())
                );
                assert_eq!(info.color_type, png::ColorType::Rgba);
                assert_eq!(info.bit_depth, png::BitDepth::Eight);
                assert_eq!(&decoded[..info.buffer_size()], expected.as_slice());
                let updates: Vec<_> = progress.try_iter().collect();
                assert!(matches!(updates[0].stage, PngExportStage::Preparing));
                assert!(matches!(
                    updates.last().unwrap().stage,
                    PngExportStage::Committing
                ));
                let encoded_rows: Vec<_> = updates
                    .iter()
                    .filter(|update| matches!(update.stage, PngExportStage::Encoding))
                    .map(|update| update.rows_written)
                    .collect();
                assert_eq!(encoded_rows.last(), Some(&snapshot.height()));
                assert!(encoded_rows.windows(2).all(|rows| rows[1] - rows[0] <= 64));
            }
        }
    }

    #[test]
    fn streamed_png_cancellation_and_commit_failure_leave_no_temporary_output() {
        let temporary = tempfile::tempdir().unwrap();
        let output = temporary.path().join("stitched.png");
        let snapshot = StitchImageSnapshot {
            canvas: TiledCanvasSnapshot::from_frame(
                Frame::new(5, 137, PixelFormat::Rgba8, vec![0x7f; 5 * 137 * 4]).unwrap(),
            ),
        };
        let (progress_tx, _) = sync_channel(16);
        assert!(matches!(
            export_png(
                snapshot.clone(),
                PngExportRequest {
                    output_path: output.clone(),
                    compression: PngCompression::Fast,
                    overwrite: false
                },
                Arc::new(AtomicBool::new(true)),
                progress_tx,
            ),
            Err(PngExportError::Canceled)
        ));
        assert_eq!(std::fs::read_dir(temporary.path()).unwrap().count(), 0);
        std::fs::write(&output, b"original").unwrap();
        let (progress_tx, _) = sync_channel(16);
        assert!(matches!(
            export_png(
                snapshot,
                PngExportRequest {
                    output_path: output.clone(),
                    compression: PngCompression::Fast,
                    overwrite: false
                },
                Arc::new(AtomicBool::new(false)),
                progress_tx,
            ),
            Err(PngExportError::Failed(_))
        ));
        assert_eq!(std::fs::read(&output).unwrap(), b"original");
        assert_eq!(std::fs::read_dir(temporary.path()).unwrap().count(), 1);
    }

    #[test]
    fn snapshot_export_writes_png_and_honors_no_clobber() {
        let temporary = tempfile::tempdir().unwrap();
        let output = temporary.path().join("stitched.png");
        let snapshot = StitchImageSnapshot {
            canvas: TiledCanvasSnapshot::from_frame(
                Frame::new(5, 5, PixelFormat::Rgba8, vec![0x7f; 5 * 5 * 4]).unwrap(),
            ),
        };
        let task = snapshot
            .export_png(PngExportRequest {
                output_path: output.clone(),
                compression: PngCompression::Best,
                overwrite: false,
            })
            .unwrap();

        task.wait().unwrap();

        assert_eq!(&std::fs::read(&output).unwrap()[..8], b"\x89PNG\r\n\x1a\n");
        assert!(
            snapshot
                .export_png(PngExportRequest {
                    output_path: output,
                    compression: PngCompression::Fast,
                    overwrite: false,
                })
                .is_err()
        );
    }
}
