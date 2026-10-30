# Recording build profiles

Standalone `snow-recording-c`, `snow-screen-recorder`, and `snow-recording-runtime`
builds enable the `legacy-capture` Cargo feature by default for compatibility.
It includes `RecordingSession`, `snow_recording_session_create`,
`snow_recording_session_stop_and_export`, and the synchronous macOS C interface.
Use `default-features = false` to omit those capture APIs. The Snow Shot FFI
archive does this and retains direct/deferred sessions, editing, trim, playback,
and old recording bundle readers and exporters.

On Windows, `ffmpeg[snow-shot-minimal]` provides the application's MP4, GIF,
APNG, and WebP outputs, including H.265/Main10 and audio. It retains Matroska
input for old `.snowrec` bundles. Standalone users that create old Matroska
intermediates or export AVI should use `ffmpeg[snow-recording-compat]`, which
extends that profile with Matroska/AVI muxing, MPEG4/MP3 encoding, and MP3 parsing
and decoding. The macOS media profile continues to include its native harness
capabilities.
