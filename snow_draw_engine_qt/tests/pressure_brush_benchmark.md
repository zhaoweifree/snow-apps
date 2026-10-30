# Pressure brush benchmark

Build `snow-canvas-pressure-brush-benchmark` with the native Release performance preset, then run
with `-platform offscreen`. The target rejects other build configurations.

```sh
cmake --build --preset build-snow-shot-macos-arm64-performance --target snow-canvas-pressure-brush-benchmark
build/snow-shot-macos-arm64-performance/snow_draw_engine_qt/snow-canvas-pressure-brush-benchmark -platform offscreen
```

The default run compares regular and pressure brushes at 512, 4096, and 16000 initial samples on a
1920×1080 logical surface. Four representative scenarios cover widths 1/8/32/72, DPR 1/2,
solid/dashed/dotted strokes, and local/full/offscreen painting. Visible traces stay within the
surface as the stroke grows. `--matrix` expands width, DPR, style, and paint policy combinations.

For a selected scenario:

```sh
snow-canvas-pressure-brush-benchmark -platform offscreen --samples 16000 --iterations 64 --batch 8 --width 8 --dpr 2 --style solid --trace noisy --paint local --mode both
```

`--surface compact` selects 640×360; `--surface full` restores 1920×1080. Explicit
`--surface-width` and `--surface-height` accept logical dimensions from 64 through 8192. Trace
choices are `straight`, `noisy`, `spiral`, and `offscreen`; mode choices are `regular`, `pressure`,
and `both`. `--device pen` supplies real varying pressure; `--device mouse` exercises simulated
pressure. `--zoom` accepts 0.1 through 8 and defaults to 1. Dimensions describe the logical surface;
physical image size includes DPR. For targeted zoom/batch checks, repeat a selected scenario with
`--zoom 0.5`, `--zoom 1`, and `--zoom 2`, paired with `--batch 1`, `--batch 8`, and `--batch 32`.

Each output line reports:

- `seed_ms`: initial input, batched geometry generation, and first display-cache synchronization.
- `cold_paint_ms` / `cold_pressure_masks`: first paint after clearing the pressure atlas for each
  case; cache counters are reset immediately afterward so live-frame metrics remain separate.
- `input_batch`, `sync`, `paint`, and `total` p50/p95: live frames after initialization; total includes
  dirty-region calculation and all three measured stages. Input batches contain new samples only.
- `ffi_payload_mean_bytes` / `ffi_payload_max_bytes`: incremental patch structures and geometry
  arrays crossing the ABI. The separate `payload_probe` timing is excluded from frame timings and
  runs afterward to avoid warming the measured synchronization.
- `retained_client_payload_bytes`: retained display-container capacity, contour/centerline path
  elements, spatial-cell payloads, and destination pixels. This is a lower bound on client memory;
  it excludes the pressure atlas, allocator overhead, Qt private path capacity, and native
  document storage. `retained_pressure_atlas_bytes` reports that separate bounded cache.
- Pressure atlas entries, chunk-mask hits, union-tile hits, rasterizations, evictions, and union-copy
  bytes describe live-frame reuse. One 16 MiB LRU budget covers chunk masks and colored union tiles
  across all canvases on the rendering thread; unchanged geometry tiles reuse their union images.
- `process_peak_rss_bytes`: process lifetime peak resident memory, including native storage. Peak
  values accumulate across scenarios; run a single scenario for an isolated memory comparison.
- Centerline/pressure chunk counts and painted-frame counts. An empty offscreen dirty region
  contributes an intentionally empty paint stage.

Interpret relative stage costs and geometry/payload growth across sample counts. There are no
wall-clock pass/fail thresholds.

## Native Qt results (2026-10-14)

Measured on the Apple M4/macOS ARM64 development machine described below, using the
`snow-shot-macos-arm64-performance` Release preset, Qt 6.12.0, and offscreen QPainter. Repository
builds and correctness tests had finished before these timing runs. The surface was 1920×1080
logical pixels. These measurements are separate from deterministic correctness tests.

The default run used 64 live frames and eight pen samples per batch. All times below are
milliseconds; each stage column is its own p95, so stage percentiles need not sum to total p95.
Cold painting is the first uncached paint and is excluded from live-frame totals.

| Initial samples | Width / DPR / style / paint | Input p95 | Sync p95 | Paint p95 | Total p95 | Cold paint |
|---:|:---|---:|---:|---:|---:|---:|
| 512 | 1 / 1 / solid / local | 0.018 | 0.008 | 0.111 | 0.130 | 0.349 |
| 512 | 8 / 2 / dashed / full | 0.021 | 0.009 | 0.773 | 0.800 | 2.136 |
| 512 | 32 / 1 / dotted / local | 0.016 | 0.007 | 0.160 | 0.177 | 0.871 |
| 4,096 | 1 / 1 / solid / local | 0.035 | 0.012 | 0.186 | 0.228 | 0.918 |
| 4,096 | 8 / 2 / dashed / full | 0.022 | 0.010 | 1.143 | 1.167 | 12.326 |
| 4,096 | 32 / 1 / dotted / local | 0.017 | 0.007 | 0.127 | 0.144 | 3.273 |
| 16,000 | 1 / 1 / solid / local | 0.014 | 0.007 | 0.216 | 0.235 | 3.028 |
| 16,000 | 8 / 2 / dashed / full | 0.033 | 0.011 | 2.288 | 2.327 | 43.509 |
| 16,000 | 32 / 1 / dotted / local | 0.023 | 0.007 | 0.304 | 0.331 | 11.107 |

At 16,000 initial samples, the expanded matrix covered widths 1/8/32/72, DPR 1/2,
solid/dashed/dotted styles, and local/full repaint policies for both brushes (96 cases,
32 frames per case). Pressure mode's worst local total p95 was **0.841 ms**; its worst full
repaint total p95 was **2.328 ms**. Both occurred with width 72, DPR 2, dashed ink. The maximum
first uncached pressure paint in that matrix was **56.098 ms**. A first paint or a new camera
transform still needs to rasterize coverage; cached live-frame timings do not describe that cost.

A separate pressure sweep used 16,000 initial samples, width 8, DPR 2, solid ink, 64 frames,
mouse/pen input, zoom 0.5/1/2, and batch sizes 1/8/32 (18 cases). Total p95 values were:

| Device / zoom | Batch 1 | Batch 8 | Batch 32 |
|:---|---:|---:|---:|
| pen / 0.5 | 0.448 | 0.516 | 0.719 |
| pen / 1 | 0.606 | 0.654 | 1.838 |
| pen / 2 | 0.552 | 0.708 | 0.878 |
| mouse / 0.5 | 0.478 | 0.553 | 0.729 |
| mouse / 1 | 0.544 | 0.563 | 0.843 |
| mouse / 2 | 0.551 | 0.720 | 0.913 |

The worst total p95 in this sweep was **1.838 ms**. The maximum first uncached paint was
**75.650 ms**. The counted pressure-atlas storage stayed below **16,777,216 bytes** in every
run. Chunk masks and completed color tiles share this budget; completed tiles receive retention
priority, and fully covered tiles skip additional masks without changing coverage. Allocator
and Qt object overhead are excluded from the counted cache budget, as described above.

Commands for the default and expanded runs were:

```sh
snow-canvas-pressure-brush-benchmark --iterations 64 -platform offscreen
snow-canvas-pressure-brush-benchmark --matrix --samples 16000 --iterations 32 -platform offscreen
```

For the sweep, use the selected-scenario command above with each combination of
`--device pen|mouse`, `--zoom 0.5|1|2`, and `--batch 1|8|32`, and `--iterations 64`.
Raw local outputs are in `build/pressure-brush-performance-final.txt`,
`build/pressure-brush-matrix-final.txt`, and `build/pressure-brush-zoom-batch-final.txt`.
They are generated measurement artifacts, not committed fixtures. Physical stylus checks and
native Windows validation were not performed on this macOS-only development host.

## Regular-brush Rust/FFI baseline

A separate isolated harness compared HEAD `d3b63aab020e` with the final pressure-brush code using
layout-matched C headers and separately built `snow-draw-engine-c` Release libraries. Current
Regular mode received real varying pen pressure and was checked after finalization to retain a
nonempty hidden profile spanning more than 0.5 pressure units. Positions, eight-event batches,
viewport configuration, and incremental patch acquisition were identical.

Hardware/toolchain: Apple M4, 10 physical cores, macOS 27.0.1, Rust 1.97.1, Apple Clang 21.0.0,
arm64. Both Rust builds used Release optimization, fat LTO, one codegen unit, and retained debug
symbols (`CARGO_PROFILE_RELEASE_DEBUG=2`); the C++ harness used `-O3`.

Each case ran five alternating paired trials, with 512 live frames and the first 32 frames excluded.
The table gives the median of the trials' total-stage p50/p95, in microseconds. This total covers
input-batch processing plus incremental patch acquisition/destruction. It excludes Qt cache
application, rasterization, and export.

| Initial samples | Width | HEAD p50 / p95 µs | Current p50 / p95 µs | p50 change |
|---:|---:|---:|---:|---:|
| 512 | 8 | 31.167 / 47.250 | 15.167 / 21.958 | -51.3% |
| 512 | 32 | 28.417 / 42.667 | 6.292 / 8.416 | -77.9% |
| 4096 | 8 | 39.000 / 100.500 | 10.166 / 21.166 | -73.9% |
| 4096 | 32 | 31.958 / 46.375 | 14.334 / 21.584 | -55.1% |
| 16000 | 8 | 59.583 / 139.459 | 7.625 / 9.917 | -87.2% |
| 16000 | 32 | 47.416 / 129.167 | 7.250 / 18.666 | -84.7% |

Runtime commands for the isolated harness were `ffi-head <initial-samples> 512 <width>` and
`ffi-current <initial-samples> 512 <width>`. The standalone libraries were built from archived
sources with:

```sh
MACOSX_DEPLOYMENT_TARGET=15.0 CARGO_PROFILE_RELEASE_DEBUG=2 CARGO_TARGET_DIR=<isolated-target> cargo build --manifest-path <archive>/Cargo.toml -p snow-draw-engine-c --target aarch64-apple-darwin --release --offline
```

The current input-stage medians were below HEAD for all six cases. The host was also running the
repository build, so absolute timings and the sub-microsecond patch stage varied. These results
establish a lower combined Rust/FFI cost for these traces; they do not independently establish a
10% bound on the patch stage or a before/after Qt paint cost. Use the native benchmark above for
surface-size, DPR, stroke-style, and repaint behavior.
