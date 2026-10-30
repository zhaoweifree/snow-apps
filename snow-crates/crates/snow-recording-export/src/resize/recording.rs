//! Recording pixels use area coverage when shrinking, so thin screen details
//! contribute to the output instead of disappearing between selected pixels.
//! Positive, exact coverage weights avoid ringing around text and high-contrast
//! edges. Native pixels and enlarged axes keep their original point samples.
use super::NearestResizePlan;
use rayon::prelude::*;
use std::ops::Range;
use std::sync::{Arc, Mutex};

/// Reusable, deterministic RGBA filter for screen-recording backgrounds.
/// Each shrinking axis integrates the complete source-pixel area covered by
/// the destination pixel. Rounding happens once after both axes are combined.
#[derive(Clone, Debug)]
pub struct RecordingResizePlan {
    source: (u32, u32),
    output: (u32, u32),
    source_len: usize,
    output_len: usize,
    nearest: Option<NearestResizePlan>,
    area: Option<AreaResizePlan>,
}

impl RecordingResizePlan {
    pub fn new(src_w: u32, src_h: u32, dst_w: u32, dst_h: u32) -> Self {
        let source = (src_w.max(1), src_h.max(1));
        let output = (dst_w.max(1), dst_h.max(1));
        let source_len = rgba_len(source);
        let output_len = rgba_len(output);
        let shrinking = output.0 < source.0 || output.1 < source.1;
        Self {
            source,
            output,
            source_len,
            output_len,
            nearest: (!shrinking && source != output)
                .then(|| NearestResizePlan::new(source.0, source.1, output.0, output.1)),
            area: shrinking.then(|| AreaResizePlan::new(source, output)),
        }
    }

    pub fn matches(&self, source: (u32, u32), output: (u32, u32)) -> bool {
        self.source == source && self.output == output
    }

    /// Resize tightly packed RGBA pixels into caller-owned reusable storage.
    /// Panics when either slice does not match the plan dimensions.
    pub fn resize_into(&self, source: &[u8], output: &mut [u8]) {
        self.resize_with_pool(source, output, None);
    }

    /// Parallel and serial filtering produce identical pixels.
    pub fn resize_into_with_pool(
        &self,
        source: &[u8],
        output: &mut [u8],
        pool: &rayon::ThreadPool,
    ) {
        self.resize_with_pool(source, output, Some(pool));
    }

    fn resize_with_pool(&self, source: &[u8], output: &mut [u8], pool: Option<&rayon::ThreadPool>) {
        self.validate_buffers(source, output);
        if let Some(area) = &self.area {
            // The general filter performs several weighted samples per pixel;
            // it benefits from bounded workers at smaller outputs than the
            // memory-bound half-size kernel.
            let parallel_pixels = if area.half { 1_000_000 } else { 262_144 };
            let parallel = pool.is_some()
                && self.output.0 as usize * self.output.1 as usize >= parallel_pixels
                && self.output.1 >= 256;
            area.resize_into(source, output, pool.filter(|_| parallel));
        } else if let Some(nearest) = &self.nearest {
            if let Some(pool) = pool {
                nearest.resize_into_with_pool(source, output, pool);
            } else {
                nearest.resize_into(source, output);
            }
        } else {
            output.copy_from_slice(source);
        }
    }

    fn validate_buffers(&self, source: &[u8], output: &[u8]) {
        assert_eq!(source.len(), self.source_len);
        assert_eq!(output.len(), self.output_len);
    }

    /// Update every output footprint touched by source damage, including pixels
    /// whose selected nearest sample lies outside that damage rectangle.
    /// Callers must establish complete damage since the output's generation.
    #[cfg(any(test, feature = "bench-experiments"))]
    pub fn resize_source_rect_into(
        &self,
        source: &[u8],
        output: &mut [u8],
        rect: (u32, u32, u32, u32),
    ) {
        self.validate_buffers(source, output);
        let (x, y, width, height) = rect;
        let right = x.saturating_add(width).min(self.source.0);
        let bottom = y.saturating_add(height).min(self.source.1);
        let left = x.min(self.source.0);
        let top = y.min(self.source.1);
        if left >= right || top >= bottom {
            return;
        }
        if let Some(area) = &self.area {
            let columns = area.x.damage_range(left as usize..right as usize);
            let rows = area.y.damage_range(top as usize..bottom as usize);
            let stride = self.output.0 as usize * 4;
            area.with_workspaces(1, |workspaces| {
                area.resize_rows(
                    source,
                    rows.start,
                    columns,
                    &mut output[rows.start * stride..rows.end * stride],
                    &mut workspaces[0],
                );
            });
        } else if let Some(nearest) = &self.nearest {
            nearest.resize_source_rect_into(source, output, rect);
        } else {
            let stride = self.source.0 as usize * 4;
            for y in top as usize..bottom as usize {
                let start = y * stride + left as usize * 4;
                let end = y * stride + right as usize * 4;
                output[start..end].copy_from_slice(&source[start..end]);
            }
        }
    }

    /// Toggle the pixel-equivalent 2:1 area kernel for benchmark comparisons.
    #[cfg(any(test, feature = "bench-experiments"))]
    pub fn set_bench_half_specialization(&mut self, enabled: bool) {
        if let Some(area) = &mut self.area {
            area.half_specialization = enabled;
        } else if let Some(nearest) = &mut self.nearest {
            nearest.set_bench_half_specialization(enabled);
        }
    }
}

fn rgba_len(size: (u32, u32)) -> usize {
    let length = (size.0 as usize)
        .checked_mul(size.1 as usize)
        .and_then(|pixels| pixels.checked_mul(4))
        .expect("recording RGBA dimensions overflow");
    assert!(
        length <= isize::MAX as usize,
        "recording RGBA buffer is too large"
    );
    length
}

#[derive(Clone, Debug)]
struct AreaResizePlan {
    source_stride: usize,
    x: AxisPlan,
    y: AxisPlan,
    average: AreaAverage,
    half: bool,
    // Each call leases its own row buffers, including concurrent calls through
    // cloned plans. The cache contains no pixels valid for a subsequent frame.
    workspaces: Arc<Mutex<Vec<AreaWorkspace>>>,
    #[cfg(any(test, feature = "bench-experiments"))]
    half_specialization: bool,
}

impl AreaResizePlan {
    fn new(source: (u32, u32), output: (u32, u32)) -> Self {
        let x = AxisPlan::new(source.0, output.0);
        let y = AxisPlan::new(source.1, output.1);
        let denominator = u64::from(x.denominator) * u64::from(y.denominator);
        Self {
            source_stride: source.0 as usize * 4,
            x,
            y,
            average: AreaAverage::new(denominator),
            half: u64::from(source.0) == u64::from(output.0) * 2
                && u64::from(source.1) == u64::from(output.1) * 2,
            workspaces: Arc::default(),
            #[cfg(any(test, feature = "bench-experiments"))]
            half_specialization: true,
        }
    }

    fn resize_into(&self, source: &[u8], output: &mut [u8], pool: Option<&rayon::ThreadPool>) {
        let row_bytes = self.x.spans.len() * 4;
        let workers = pool
            .map_or(1, rayon::ThreadPool::current_num_threads)
            .min(self.y.spans.len());
        let rows_per_worker = self.y.spans.len().div_ceil(workers);
        let chunk_bytes = row_bytes * rows_per_worker;
        self.with_workspaces(workers, |workspaces| {
            let resize =
                |(index, (output, workspace)): (usize, (&mut [u8], &mut AreaWorkspace))| {
                    self.resize_rows(
                        source,
                        index * rows_per_worker,
                        0..self.x.spans.len(),
                        output,
                        workspace,
                    );
                };
            if let Some(pool) = pool {
                pool.install(|| {
                    output
                        .par_chunks_mut(chunk_bytes)
                        .zip(workspaces.par_iter_mut())
                        .enumerate()
                        .for_each(resize);
                });
            } else {
                resize((0, (output, &mut workspaces[0])));
            }
        });
    }

    fn with_workspaces(&self, count: usize, run: impl FnOnce(&mut [AreaWorkspace])) {
        let mut workspaces = std::mem::take(&mut *self.workspaces.lock().unwrap());
        // Retain worker buffers when a guarded frame falls back to serial.
        if workspaces.len() < count {
            workspaces.resize_with(count, AreaWorkspace::default);
        }
        run(&mut workspaces[..count]);
        let mut cache = self.workspaces.lock().unwrap();
        if cache.len() < workspaces.len() {
            *cache = workspaces;
        }
    }

    fn resize_rows(
        &self,
        source: &[u8],
        first_y: usize,
        columns: Range<usize>,
        output: &mut [u8],
        workspace: &mut AreaWorkspace,
    ) {
        #[cfg(any(test, feature = "bench-experiments"))]
        let half = self.half && self.half_specialization;
        #[cfg(not(any(test, feature = "bench-experiments")))]
        let half = self.half;
        let stride = self.x.spans.len() * 4;
        if half {
            for (dy, output) in output.chunks_exact_mut(stride).enumerate() {
                let y = first_y + dy;
                let first = y * 2 * self.source_stride;
                let second = first + self.source_stride;
                resize_half_row(
                    &source[first..first + self.source_stride],
                    &source[second..second + self.source_stride],
                    columns.clone(),
                    output,
                );
            }
            return;
        }

        if let Some(reciprocal) = self.average.reciprocal32 {
            self.resize_rows_narrow(
                source,
                first_y,
                columns,
                output,
                workspace,
                reciprocal,
                NarrowKernels::new(),
            );
            return;
        }

        workspace.horizontal.resize(columns.len(), [0; 4]);
        workspace.vertical.resize(columns.len(), [0; 4]);
        let mut cached_row = None;
        for (dy, output) in output.chunks_exact_mut(stride).enumerate() {
            let y_span = &self.y.spans[first_y + dy];
            let y_weights = &self.y.weights[y_span.weights.clone()];
            workspace.vertical.fill([0; 4]);
            for (dy, &weight) in y_weights.iter().enumerate() {
                let source_y = y_span.first + dy;
                if cached_row != Some(source_y) {
                    let row =
                        &source[source_y * self.source_stride..(source_y + 1) * self.source_stride];
                    self.resize_horizontal(row, columns.clone(), &mut workspace.horizontal);
                    cached_row = Some(source_y);
                }
                for (sum, horizontal) in workspace.vertical.iter_mut().zip(&workspace.horizontal) {
                    for channel in 0..4 {
                        sum[channel] += horizontal[channel] * u64::from(weight);
                    }
                }
            }
            for (pixel, sums) in output[columns.start * 4..columns.end * 4]
                .chunks_exact_mut(4)
                .zip(&workspace.vertical)
            {
                for (channel, &sum) in pixel.iter_mut().zip(sums) {
                    *channel = self.average.round(sum);
                }
            }
        }
    }

    fn resize_rows_narrow(
        &self,
        source: &[u8],
        first_y: usize,
        columns: Range<usize>,
        output: &mut [u8],
        workspace: &mut AreaWorkspace,
        reciprocal: u32,
        kernels: NarrowKernels,
    ) {
        let horizontal = &mut workspace.horizontal32;
        let vertical = &mut workspace.vertical32;
        horizontal.resize(columns.len(), [0; 4]);
        vertical.resize(columns.len(), [0; 4]);
        let mut cached_row = None;
        let denominator = self.average.denominator as u32;
        for (dy, output) in output.chunks_exact_mut(self.x.spans.len() * 4).enumerate() {
            let y_span = &self.y.spans[first_y + dy];
            let y_weights = &self.y.weights[y_span.weights.clone()];
            vertical.fill([0; 4]);
            for (dy, &weight) in y_weights.iter().enumerate() {
                let source_y = y_span.first + dy;
                if cached_row != Some(source_y) {
                    let row =
                        &source[source_y * self.source_stride..(source_y + 1) * self.source_stride];
                    // SAFETY: kernels are selected only after checking CPU
                    // features. The plan validates every row and footprint;
                    // its denominator bounds all channel sums to u32.
                    unsafe {
                        (kernels.horizontal)(&self.x, row, columns.clone(), horizontal);
                    }
                    cached_row = Some(source_y);
                }
                // SAFETY: both rows have columns.len() pixels and the total
                // two-axis weight bounds each accumulated channel to u32.
                unsafe {
                    (kernels.accumulate)(horizontal, vertical, weight);
                }
            }
            // SAFETY: the destination has four bytes per sum and the bound
            // also covers the rounding numerator and quotient correction.
            unsafe {
                (kernels.write)(
                    vertical,
                    &mut output[columns.start * 4..columns.end * 4],
                    denominator,
                    reciprocal,
                );
            }
        }
    }

    fn resize_horizontal(&self, source: &[u8], columns: Range<usize>, output: &mut [[u64; 4]]) {
        for (sum, x) in output.iter_mut().zip(columns) {
            let x_span = &self.x.spans[x];
            let x_weights = &self.x.weights[x_span.weights.clone()];
            // Two 32-bit channel lanes share each multiplication. The total
            // weight bounds every lane, so neither products nor sums can carry
            // into the adjacent channel. Larger geometries use wide sums below.
            let pixels = &source[x_span.first * 4..(x_span.first + x_weights.len()) * 4];
            if u64::from(self.x.denominator) <= u64::from(u32::MAX) / 256 {
                let mut red_blue = 0_u64;
                let mut green_alpha = 0_u64;
                for (pixel, &wx) in pixels.chunks_exact(4).zip(x_weights) {
                    let pixel = u64::from(u32::from_le_bytes(pixel.try_into().unwrap()));
                    let weight = u64::from(wx);
                    red_blue += ((pixel & 255) | ((pixel & 0x00ff_0000) << 16)) * weight;
                    green_alpha += ((pixel >> 8 & 255) | ((pixel >> 24) << 32)) * weight;
                }
                *sum = [
                    red_blue & u64::from(u32::MAX),
                    green_alpha & u64::from(u32::MAX),
                    red_blue >> 32,
                    green_alpha >> 32,
                ];
                continue;
            }
            sum.fill(0);
            for (pixel, &weight) in pixels.chunks_exact(4).zip(x_weights) {
                for (sum, &value) in sum.iter_mut().zip(pixel) {
                    *sum += u64::from(value) * u64::from(weight);
                }
            }
        }
    }
}

#[derive(Debug, Default)]
struct AreaWorkspace {
    horizontal: Vec<[u64; 4]>,
    vertical: Vec<[u64; 4]>,
    horizontal32: Vec<[u32; 4]>,
    vertical32: Vec<[u32; 4]>,
}

type HorizontalNarrowKernel = unsafe fn(&AxisPlan, &[u8], Range<usize>, &mut [[u32; 4]]);
type AccumulateNarrowKernel = unsafe fn(&[[u32; 4]], &mut [[u32; 4]], u32);
type WriteNarrowKernel = unsafe fn(&[[u32; 4]], &mut [u8], u32, u32);

#[derive(Clone, Copy)]
struct NarrowKernels {
    horizontal: HorizontalNarrowKernel,
    accumulate: AccumulateNarrowKernel,
    write: WriteNarrowKernel,
}

impl NarrowKernels {
    fn new() -> Self {
        #[cfg(target_arch = "aarch64")]
        if std::arch::is_aarch64_feature_detected!("neon") {
            return Self {
                horizontal: horizontal_neon,
                accumulate: accumulate_neon,
                write: write_neon,
            };
        }
        #[cfg(target_arch = "x86_64")]
        if std::arch::is_x86_feature_detected!("sse4.1") {
            if std::arch::is_x86_feature_detected!("avx2") {
                return Self {
                    horizontal: horizontal_sse41,
                    accumulate: accumulate_avx2,
                    write: write_avx2,
                };
            }
            return Self {
                horizontal: horizontal_sse41,
                accumulate: accumulate_sse41,
                write: write_sse41,
            };
        }
        Self::portable()
    }

    fn portable() -> Self {
        Self {
            horizontal: horizontal_narrow,
            accumulate: accumulate_narrow,
            write: write_narrow,
        }
    }
}

fn horizontal_narrow(axis: &AxisPlan, row: &[u8], columns: Range<usize>, output: &mut [[u32; 4]]) {
    for (sum, x) in output.iter_mut().zip(columns) {
        let span = &axis.spans[x];
        let weights = &axis.weights[span.weights.clone()];
        let pixels = &row[span.first * 4..(span.first + weights.len()) * 4];
        sum.fill(0);
        for (pixel, &weight) in pixels.chunks_exact(4).zip(weights) {
            for channel in 0..4 {
                sum[channel] += u32::from(pixel[channel]) * weight;
            }
        }
    }
}

fn accumulate_narrow(source: &[[u32; 4]], output: &mut [[u32; 4]], weight: u32) {
    for (sum, horizontal) in output.iter_mut().zip(source) {
        for channel in 0..4 {
            sum[channel] += horizontal[channel] * weight;
        }
    }
}

fn write_narrow(source: &[[u32; 4]], output: &mut [u8], denominator: u32, reciprocal: u32) {
    if denominator.is_power_of_two() {
        let shift = denominator.trailing_zeros();
        for (pixel, sums) in output.chunks_exact_mut(4).zip(source) {
            for (channel, &sum) in pixel.iter_mut().zip(sums) {
                *channel = ((sum + denominator / 2) >> shift) as u8;
            }
        }
        return;
    }
    for (pixel, sums) in output.chunks_exact_mut(4).zip(source) {
        for (channel, &sum) in pixel.iter_mut().zip(sums) {
            let numerator = sum + denominator / 2;
            let estimate = ((u64::from(numerator) * u64::from(reciprocal)) >> 32) as u32;
            *channel = (estimate - u32::from(estimate * denominator > numerator)) as u8;
        }
    }
}

#[cfg(target_arch = "aarch64")]
#[target_feature(enable = "neon")]
unsafe fn horizontal_neon(
    axis: &AxisPlan,
    row: &[u8],
    columns: Range<usize>,
    output: &mut [[u32; 4]],
) {
    use std::arch::aarch64::*;
    if let Some(short) = &axis.short {
        // SAFETY: plans record complete in-bounds pixels, including the
        // zero-weight padding taps. AArch64 provides baseline NEON.
        unsafe {
            match axis.short_taps {
                1 => horizontal_short_neon::<1>(&short[columns], row, output),
                2 => horizontal_short_neon::<2>(&short[columns], row, output),
                3 => horizontal_short_neon::<3>(&short[columns], row, output),
                4 => horizontal_short_neon::<4>(&short[columns], row, output),
                _ => unreachable!("short footprints have at most four taps"),
            }
        }
        return;
    }
    for (destination, x) in output.iter_mut().zip(columns) {
        let span = &axis.spans[x];
        let weights = &axis.weights[span.weights.clone()];
        let pixels = &row[span.first * 4..(span.first + weights.len()) * 4];
        let mut sum = vdupq_n_u32(0);
        for (pixel, &weight) in pixels.chunks_exact(4).zip(weights) {
            let bytes = vcreate_u8(u64::from(u32::from_le_bytes(pixel.try_into().unwrap())));
            let rgba = vmovl_u16(vget_low_u16(vmovl_u8(bytes)));
            sum = vmlaq_n_u32(sum, rgba, weight);
        }
        // SAFETY: each destination owns four u32 channels. The narrow-plan
        // denominator bounds every weighted sum to its unsigned channel lane.
        unsafe { vst1q_u32(destination.as_mut_ptr(), sum) };
    }
}

#[cfg(target_arch = "aarch64")]
#[target_feature(enable = "neon")]
unsafe fn horizontal_short_neon<const TAPS: usize>(
    spans: &[ShortFootprint],
    row: &[u8],
    output: &mut [[u32; 4]],
) {
    use std::arch::aarch64::*;
    for (span, destination) in spans.iter().zip(output) {
        let mut sum = vdupq_n_u32(0);
        for tap in 0..TAPS {
            // SAFETY: even absent taps point at a complete pixel in this row.
            let pixel = unsafe {
                row.as_ptr()
                    .add(span.offsets[tap])
                    .cast::<u32>()
                    .read_unaligned()
            };
            let rgba = vmovl_u16(vget_low_u16(vmovl_u8(vcreate_u8(u64::from(pixel)))));
            sum = vmlaq_n_u32(sum, rgba, span.weights[tap]);
        }
        // SAFETY: the destination contains exactly four writable channels.
        unsafe { vst1q_u32(destination.as_mut_ptr(), sum) };
    }
}

#[cfg(target_arch = "aarch64")]
#[target_feature(enable = "neon")]
unsafe fn accumulate_neon(source: &[[u32; 4]], output: &mut [[u32; 4]], weight: u32) {
    use std::arch::aarch64::*;
    for (source, destination) in source.iter().zip(output) {
        // SAFETY: each array has four initialized channels, and narrow-plan
        // bounds ensure all products and accumulated sums fit in u32.
        unsafe {
            let value = vld1q_u32(source.as_ptr());
            let sum = vld1q_u32(destination.as_ptr());
            vst1q_u32(destination.as_mut_ptr(), vmlaq_n_u32(sum, value, weight));
        }
    }
}

#[cfg(target_arch = "aarch64")]
#[target_feature(enable = "neon")]
unsafe fn write_neon(source: &[[u32; 4]], output: &mut [u8], denominator: u32, reciprocal: u32) {
    use std::arch::aarch64::*;
    let half = vdupq_n_u32(denominator / 2);
    let shift = vdupq_n_s32(-(denominator.trailing_zeros() as i32));
    for (sum, pixel) in source.iter().zip(output.chunks_exact_mut(4)) {
        // SAFETY: each source contains four initialized u32 channels.
        let numerator = vaddq_u32(unsafe { vld1q_u32(sum.as_ptr()) }, half);
        let rounded = if denominator.is_power_of_two() {
            vshlq_u32(numerator, shift)
        } else {
            let low = vshrn_n_u64::<32>(vmull_n_u32(vget_low_u32(numerator), reciprocal));
            let high = vshrn_n_u64::<32>(vmull_high_n_u32(numerator, reciprocal));
            let estimate = vcombine_u32(low, high);
            let product = vmulq_n_u32(estimate, denominator);
            let correction = vshrq_n_u32::<31>(vcgtq_u32(product, numerator));
            vsubq_u32(estimate, correction)
        };
        // Exact reciprocal correction bounds the result to an 8-bit channel.
        let words = vmovn_u32(rounded);
        let bytes = vmovn_u16(vcombine_u16(words, vdup_n_u16(0)));
        pixel.copy_from_slice(&vget_lane_u32::<0>(vreinterpret_u32_u8(bytes)).to_le_bytes());
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sse4.1")]
unsafe fn horizontal_sse41(
    axis: &AxisPlan,
    row: &[u8],
    columns: Range<usize>,
    output: &mut [[u32; 4]],
) {
    use std::arch::x86_64::*;
    if let Some(short) = &axis.short {
        let short = &short[columns];
        // SAFETY: this entry point requires SSE4.1. Axis construction records
        // only in-bounds pixel offsets, including padding for absent taps.
        unsafe {
            match axis.short_taps {
                1 => horizontal_short_sse41::<1>(short, row, output),
                2 => horizontal_short_sse41::<2>(short, row, output),
                3 => horizontal_short_sse41::<3>(short, row, output),
                4 => horizontal_short_sse41::<4>(short, row, output),
                _ => unreachable!("short footprints have at most four taps"),
            }
        }
        return;
    }
    for (destination, x) in output.iter_mut().zip(columns) {
        let span = &axis.spans[x];
        let weights = &axis.weights[span.weights.clone()];
        let pixels = &row[span.first * 4..(span.first + weights.len()) * 4];
        let mut sum = _mm_setzero_si128();
        for (pixel, &weight) in pixels.chunks_exact(4).zip(weights) {
            let pixel = u32::from_le_bytes(pixel.try_into().unwrap());
            let rgba = _mm_cvtepu8_epi32(_mm_cvtsi32_si128(pixel as i32));
            sum = _mm_add_epi32(sum, _mm_mullo_epi32(rgba, _mm_set1_epi32(weight as i32)));
        }
        // SAFETY: each array owns sixteen writable bytes; unaligned stores
        // support every allocation and damage-column offset.
        unsafe { _mm_storeu_si128(destination.as_mut_ptr().cast(), sum) };
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sse4.1")]
unsafe fn horizontal_short_sse41<const TAPS: usize>(
    spans: &[ShortFootprint],
    row: &[u8],
    output: &mut [[u32; 4]],
) {
    use std::arch::x86_64::*;
    for (span, destination) in spans.iter().zip(output) {
        let mut sum = _mm_setzero_si128();
        for tap in 0..TAPS {
            // SAFETY: every offset covers a complete pixel in this validated
            // source row. Zero-weight taps reuse the first valid offset.
            let pixel = unsafe {
                row.as_ptr()
                    .add(span.offsets[tap])
                    .cast::<i32>()
                    .read_unaligned()
            };
            let rgba = _mm_cvtepu8_epi32(_mm_cvtsi32_si128(pixel));
            sum = _mm_add_epi32(
                sum,
                _mm_mullo_epi32(rgba, _mm_set1_epi32(span.weights[tap] as i32)),
            );
        }
        // SAFETY: each destination owns sixteen bytes.
        unsafe { _mm_storeu_si128(destination.as_mut_ptr().cast(), sum) };
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sse4.1")]
unsafe fn accumulate_sse41(source: &[[u32; 4]], output: &mut [[u32; 4]], weight: u32) {
    use std::arch::x86_64::*;
    let weight = _mm_set1_epi32(weight as i32);
    for (source, destination) in source.iter().zip(output) {
        // SAFETY: every array contains four initialized u32 channels. Narrow
        // dispatch guarantees that weighted products and sums fit those lanes.
        unsafe {
            let source = _mm_loadu_si128(source.as_ptr().cast());
            let sum = _mm_loadu_si128(destination.as_ptr().cast());
            _mm_storeu_si128(
                destination.as_mut_ptr().cast(),
                _mm_add_epi32(sum, _mm_mullo_epi32(source, weight)),
            );
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sse4.1")]
unsafe fn write_sse41(source: &[[u32; 4]], output: &mut [u8], denominator: u32, reciprocal: u32) {
    use std::arch::x86_64::*;
    let half = _mm_set1_epi32((denominator / 2) as i32);
    let zero = _mm_setzero_si128();
    if denominator.is_power_of_two() {
        let shift = _mm_cvtsi32_si128(denominator.trailing_zeros() as i32);
        for (sum, pixel) in source.iter().zip(output.chunks_exact_mut(4)) {
            // SAFETY: every source array owns sixteen initialized bytes.
            let sum = unsafe { _mm_loadu_si128(sum.as_ptr().cast()) };
            let rounded = _mm_srl_epi32(_mm_add_epi32(sum, half), shift);
            let bytes = _mm_packus_epi16(_mm_packus_epi32(rounded, zero), zero);
            pixel.copy_from_slice(&(_mm_cvtsi128_si32(bytes) as u32).to_le_bytes());
        }
        return;
    }
    let reciprocal = _mm_set1_epi32(reciprocal as i32);
    let denominator = _mm_set1_epi32(denominator as i32);
    let sign = _mm_set1_epi32(i32::MIN);
    for (sum, pixel) in source.iter().zip(output.chunks_exact_mut(4)) {
        // SAFETY: each source array contains sixteen initialized bytes.
        let sum = unsafe { _mm_loadu_si128(sum.as_ptr().cast()) };
        let numerator = _mm_add_epi32(sum, half);
        let even = _mm_srli_epi64::<32>(_mm_mul_epu32(numerator, reciprocal));
        let odd = _mm_srli_epi64::<32>(_mm_mul_epu32(_mm_srli_si128::<4>(numerator), reciprocal));
        let estimate = _mm_unpacklo_epi32(
            _mm_shuffle_epi32::<0x88>(even),
            _mm_shuffle_epi32::<0x88>(odd),
        );
        let product = _mm_mullo_epi32(estimate, denominator);
        // Flipping the sign bit implements unsigned comparison. The reciprocal
        // may overestimate by one; subtract that exact correction before packing.
        let correction =
            _mm_cmpgt_epi32(_mm_xor_si128(product, sign), _mm_xor_si128(numerator, sign));
        let rounded = _mm_add_epi32(estimate, correction);
        let bytes = _mm_packus_epi16(_mm_packus_epi32(rounded, zero), zero);
        pixel.copy_from_slice(&(_mm_cvtsi128_si32(bytes) as u32).to_le_bytes());
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,sse4.1")]
unsafe fn accumulate_avx2(source: &[[u32; 4]], output: &mut [[u32; 4]], weight: u32) {
    use std::arch::x86_64::*;
    let weights = _mm256_set1_epi32(weight as i32);
    let mut source = source.chunks_exact(2);
    let mut output = output.chunks_exact_mut(2);
    for (source, destination) in source.by_ref().zip(output.by_ref()) {
        // SAFETY: each two-pixel chunk owns thirty-two bytes. Narrow dispatch
        // bounds every product and sum to its unsigned 32-bit channel lane.
        unsafe {
            let source = _mm256_loadu_si256(source.as_ptr().cast());
            let sum = _mm256_loadu_si256(destination.as_ptr().cast());
            _mm256_storeu_si256(
                destination.as_mut_ptr().cast(),
                _mm256_add_epi32(sum, _mm256_mullo_epi32(source, weights)),
            );
        }
    }
    // SAFETY: feature selection checked SSE4.1 too; the remainder has at most
    // one complete pixel and the same bounded arithmetic contract.
    unsafe { accumulate_sse41(source.remainder(), output.into_remainder(), weight) };
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,sse4.1")]
unsafe fn write_avx2(source: &[[u32; 4]], output: &mut [u8], denominator: u32, reciprocal: u32) {
    use std::arch::x86_64::*;
    let half = _mm256_set1_epi32((denominator / 2) as i32);
    let reciprocal_vector = _mm256_set1_epi32(reciprocal as i32);
    let denominator_vector = _mm256_set1_epi32(denominator as i32);
    let sign = _mm256_set1_epi32(i32::MIN);
    let zero = _mm_setzero_si128();
    let shift = _mm_cvtsi32_si128(denominator.trailing_zeros() as i32);
    let mut source = source.chunks_exact(2);
    let mut output = output.chunks_exact_mut(8);
    for (sums, pixels) in source.by_ref().zip(output.by_ref()) {
        // SAFETY: the chunk contains two complete four-channel sum arrays.
        let sums = unsafe { _mm256_loadu_si256(sums.as_ptr().cast()) };
        let numerator = _mm256_add_epi32(sums, half);
        let rounded = if denominator.is_power_of_two() {
            _mm256_srl_epi32(numerator, shift)
        } else {
            let even = _mm256_srli_epi64::<32>(_mm256_mul_epu32(numerator, reciprocal_vector));
            let odd = _mm256_srli_epi64::<32>(_mm256_mul_epu32(
                _mm256_srli_si256::<4>(numerator),
                reciprocal_vector,
            ));
            let estimate = _mm256_unpacklo_epi32(
                _mm256_shuffle_epi32::<0x88>(even),
                _mm256_shuffle_epi32::<0x88>(odd),
            );
            let product = _mm256_mullo_epi32(estimate, denominator_vector);
            let correction = _mm256_cmpgt_epi32(
                _mm256_xor_si256(product, sign),
                _mm256_xor_si256(numerator, sign),
            );
            _mm256_add_epi32(estimate, correction)
        };
        let words = _mm_packus_epi32(
            _mm256_castsi256_si128(rounded),
            _mm256_extracti128_si256::<1>(rounded),
        );
        let bytes = _mm_packus_epi16(words, zero);
        pixels.copy_from_slice(&(_mm_cvtsi128_si64(bytes) as u64).to_le_bytes());
    }
    // SAFETY: feature selection checked SSE4.1 and the tail contains at most
    // one complete pixel, with the same rounding numerator/product bounds.
    unsafe {
        write_sse41(
            source.remainder(),
            output.into_remainder(),
            denominator,
            reciprocal,
        )
    };
}

/// Exact rounded division for channel sums, without division in the frame loop.
#[derive(Clone, Debug)]
struct AreaAverage {
    denominator: u64,
    reciprocal: u64,
    reciprocal32: Option<u32>,
}

impl AreaAverage {
    fn new(denominator: u64) -> Self {
        // Area filtering has at least one shrinking axis, so its denominator
        // is at least two. This also makes the reciprocal representable in u64.
        assert!((2..=u64::MAX / 256).contains(&denominator));
        Self {
            denominator,
            reciprocal: (1u128 << 64).div_ceil(u128::from(denominator)) as u64,
            reciprocal32: (denominator <= u64::from(u32::MAX) / 256)
                .then(|| (1_u64 << 32).div_ceil(denominator) as u32),
        }
    }

    #[inline]
    fn round(&self, sum: u64) -> u8 {
        let numerator = sum + self.denominator / 2;
        // ceil(2^64 / d) gives a quotient estimate no smaller than n / d.
        // Because n < 2^64, the reciprocal's rounding error adds less than one
        // to the quotient. One comparison therefore corrects it exactly.
        let estimate = ((u128::from(numerator) * u128::from(self.reciprocal)) >> 64) as u64;
        // Channel sums are at most 255*d, so estimate <= 256. The constructor
        // bound keeps both numerator and this correction product in u64.
        (estimate - u64::from(estimate * self.denominator > numerator)) as u8
    }
}

fn resize_half_row(top: &[u8], bottom: &[u8], columns: Range<usize>, output: &mut [u8]) {
    debug_assert_eq!(top.len(), output.len() * 2);
    debug_assert_eq!(bottom.len(), top.len());
    debug_assert!(columns.end <= output.len() / 4);
    #[cfg(target_arch = "aarch64")]
    // SAFETY: AArch64 provides NEON and the validated row slices cover every
    // selected output pixel.
    let mut x = unsafe { resize_half_neon(top, bottom, columns.clone(), output) };
    #[cfg(not(target_arch = "aarch64"))]
    let mut x = columns.start;
    #[cfg(target_arch = "x86_64")]
    {
        use std::arch::x86_64::*;
        while x + 4 <= columns.end {
            let from = x * 8;
            // SAFETY: SSE2 is mandatory on x86-64. These four output pixels
            // cover eight in-bounds pixels from each validated source row.
            // Unaligned loads/stores allow arbitrary row and damage offsets.
            unsafe {
                let first = half_row_pair(
                    _mm_loadu_si128(top.as_ptr().add(from).cast()),
                    _mm_loadu_si128(bottom.as_ptr().add(from).cast()),
                );
                let second = half_row_pair(
                    _mm_loadu_si128(top.as_ptr().add(from + 16).cast()),
                    _mm_loadu_si128(bottom.as_ptr().add(from + 16).cast()),
                );
                _mm_storeu_si128(
                    output.as_mut_ptr().add(x * 4).cast(),
                    _mm_packus_epi16(first, second),
                );
            }
            x += 4;
        }
    }
    while x < columns.end {
        let from = x * 8;
        let to = x * 4;
        for channel in 0..4 {
            let sum = u16::from(top[from + channel])
                + u16::from(top[from + 4 + channel])
                + u16::from(bottom[from + channel])
                + u16::from(bottom[from + 4 + channel]);
            output[to + channel] = ((sum + 2) / 4) as u8;
        }
        x += 1;
    }
}

#[cfg(target_arch = "aarch64")]
#[target_feature(enable = "neon")]
unsafe fn resize_half_neon(
    top: &[u8],
    bottom: &[u8],
    columns: Range<usize>,
    output: &mut [u8],
) -> usize {
    use std::arch::aarch64::*;
    let mut x = columns.start;
    while x + 4 <= columns.end {
        // SAFETY: four output pixels consume eight complete pixels in each
        // source row. Interleaved loads split even and odd RGBA pixels without
        // reading outside those 32 bytes; all accesses permit unaligned rows.
        unsafe {
            let first = vld2q_u32(top.as_ptr().add(x * 8).cast());
            let second = vld2q_u32(bottom.as_ptr().add(x * 8).cast());
            let a = vreinterpretq_u8_u32(first.0);
            let b = vreinterpretq_u8_u32(first.1);
            let c = vreinterpretq_u8_u32(second.0);
            let d = vreinterpretq_u8_u32(second.1);
            let low = vaddq_u16(
                vaddl_u8(vget_low_u8(a), vget_low_u8(b)),
                vaddl_u8(vget_low_u8(c), vget_low_u8(d)),
            );
            let high = vaddq_u16(vaddl_high_u8(a, b), vaddl_high_u8(c, d));
            let pixels = vcombine_u8(vrshrn_n_u16::<2>(low), vrshrn_n_u16::<2>(high));
            vst1q_u8(output.as_mut_ptr().add(x * 4), pixels);
        }
        x += 4;
    }
    x
}

#[cfg(target_arch = "x86_64")]
#[inline]
fn half_row_pair(
    top: std::arch::x86_64::__m128i,
    bottom: std::arch::x86_64::__m128i,
) -> std::arch::x86_64::__m128i {
    use std::arch::x86_64::*;
    // SAFETY: SSE2 is mandatory on x86-64; all operations are register-only.
    unsafe {
        let zero = _mm_setzero_si128();
        let low = _mm_add_epi16(
            _mm_unpacklo_epi8(top, zero),
            _mm_unpacklo_epi8(bottom, zero),
        );
        let high = _mm_add_epi16(
            _mm_unpackhi_epi8(top, zero),
            _mm_unpackhi_epi8(bottom, zero),
        );
        let low = _mm_add_epi16(low, _mm_srli_si128::<8>(low));
        let high = _mm_add_epi16(high, _mm_srli_si128::<8>(high));
        let sums = _mm_unpacklo_epi64(low, high);
        _mm_srli_epi16::<2>(_mm_add_epi16(sums, _mm_set1_epi16(2)))
    }
}

#[derive(Clone, Debug)]
struct AxisPlan {
    spans: Vec<AxisSpan>,
    weights: Vec<u32>,
    denominator: u32,
    #[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
    short: Option<Vec<ShortFootprint>>,
    #[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
    short_taps: usize,
}

#[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
#[derive(Clone, Debug)]
struct ShortFootprint {
    offsets: [usize; 4],
    weights: [u32; 4],
}

#[derive(Clone, Debug)]
struct AxisSpan {
    first: usize,
    weights: Range<usize>,
}

impl AxisPlan {
    fn new(source: u32, output: u32) -> Self {
        let mut divisor = source;
        let mut remainder = output;
        while remainder != 0 {
            (divisor, remainder) = (remainder, divisor % remainder);
        }
        let mut spans = Vec::with_capacity(output as usize);
        let mut weights = Vec::new();
        for destination in 0..output {
            let start_weight = weights.len();
            let first;
            if output < source {
                // Work in output-sized integer units: each source cell covers
                // `output` units and each destination footprint `source` units.
                let left = u64::from(destination) * u64::from(source);
                let right = (u64::from(destination) + 1) * u64::from(source);
                first = (left / u64::from(output)) as usize;
                let end = right.div_ceil(u64::from(output)) as usize;
                for sample in first..end {
                    let sample_left = sample as u64 * u64::from(output);
                    let sample_right = sample_left + u64::from(output);
                    weights
                        .push((right.min(sample_right) - left.max(sample_left)) as u32 / divisor);
                }
            } else {
                first = (u64::from(destination) * u64::from(source) / u64::from(output)) as usize;
                weights.push(1);
            }
            spans.push(AxisSpan {
                first,
                weights: start_weight..weights.len(),
            });
        }
        #[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
        let short_taps = spans
            .iter()
            .map(|span| span.weights.len())
            .max()
            .unwrap_or(1);
        #[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
        let short = (short_taps <= 4).then(|| {
            spans
                .iter()
                .map(|span| {
                    let mut footprint = ShortFootprint {
                        offsets: [span.first * 4; 4],
                        weights: [0; 4],
                    };
                    for (tap, &weight) in weights[span.weights.clone()].iter().enumerate() {
                        footprint.offsets[tap] = (span.first + tap) * 4;
                        footprint.weights[tap] = weight;
                    }
                    footprint
                })
                .collect()
        });
        Self {
            spans,
            weights,
            denominator: if output < source { source / divisor } else { 1 },
            #[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
            short,
            #[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
            short_taps,
        }
    }

    #[cfg(any(test, feature = "bench-experiments"))]
    fn damage_range(&self, damage: Range<usize>) -> Range<usize> {
        let first = self
            .spans
            .partition_point(|span| span.first + span.weights.len() <= damage.start);
        let end = self.spans.partition_point(|span| span.first < damage.end);
        first..end
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn coverage(sample: u32, destination: u32, source: u32, output: u32) -> u64 {
        if output >= source {
            return u64::from(sample == destination * source / output);
        }
        let left = u64::from(destination) * u64::from(source);
        let right = left + u64::from(source);
        let sample_left = u64::from(sample) * u64::from(output);
        let sample_right = sample_left + u64::from(output);
        right
            .min(sample_right)
            .saturating_sub(left.max(sample_left))
    }

    // Brute-force every source cell, independent of planned spans, normalized
    // weights, row caching, separable intermediates, and vector arithmetic.
    fn area_reference(source: &[u8], input: (u32, u32), output: (u32, u32)) -> Vec<u8> {
        let denominator = u64::from(if output.0 < input.0 { input.0 } else { 1 })
            * u64::from(if output.1 < input.1 { input.1 } else { 1 });
        let mut result = Vec::new();
        for y in 0..output.1 {
            for x in 0..output.0 {
                let mut sums = [0_u64; 4];
                for sy in 0..input.1 {
                    for sx in 0..input.0 {
                        let weight =
                            coverage(sx, x, input.0, output.0) * coverage(sy, y, input.1, output.1);
                        let from = ((sy * input.0 + sx) * 4) as usize;
                        for channel in 0..4 {
                            sums[channel] += u64::from(source[from + channel]) * weight;
                        }
                    }
                }
                result.extend(sums.map(|sum| ((sum + denominator / 2) / denominator) as u8));
            }
        }
        result
    }

    #[test]
    fn separable_filter_matches_independent_coverage_for_color_and_reused_frames() {
        for (input, output) in [
            ((17, 13), (11, 7)),
            ((17, 13), (3, 2)),
            ((13, 17), (7, 11)),
            ((12, 9), (8, 6)),
            ((14, 10), (7, 5)),
            ((13, 9), (6, 4)),
            ((9, 13), (4, 6)),
            ((3, 7), (8, 4)),
            ((7, 3), (4, 8)),
            ((9, 7), (1, 1)),
            ((1, 17), (1, 7)),
            ((17, 1), (7, 1)),
            ((8, 5), (8, 3)),
            ((8, 5), (3, 5)),
        ] {
            let mut plan = RecordingResizePlan::new(input.0, input.1, output.0, output.1);
            plan.set_bench_half_specialization(false);
            for frame in 0..4 {
                let source: Vec<_> = (0..input.0 * input.1 * 4)
                    .map(|index| ((index * 37 + frame * 89) % 256) as u8)
                    .collect();
                let expected = area_reference(&source, input, output);
                let mut actual = vec![0xa5; expected.len()];
                plan.resize_into(&source, &mut actual);
                assert_eq!(actual, expected, "{input:?}->{output:?}, frame {frame}");
                let area = plan.area.as_ref().unwrap();
                for kernels in available_narrow_kernels() {
                    area.with_workspaces(1, |workspaces| {
                        area.resize_rows_narrow(
                            &source,
                            0,
                            0..output.0 as usize,
                            &mut actual,
                            &mut workspaces[0],
                            area.average.reciprocal32.unwrap(),
                            kernels,
                        );
                    });
                    assert_eq!(actual, expected, "narrow {input:?}->{output:?}");
                }
                // Force wide arithmetic on the same small fixture to validate
                // that it preserves the independent coverage contract too.
                let mut wide = plan.clone();
                wide.area.as_mut().unwrap().average.reciprocal32 = None;
                wide.resize_into(&source, &mut actual);
                assert_eq!(actual, expected, "wide {input:?}->{output:?}");
            }
        }
    }

    #[test]
    fn all_small_ratios_match_cell_coverage_without_edge_ringing() {
        // Coprime, integer, one-axis and mixed ratios exercise different tap
        // footprints. Distinct channel ranges expose channel-lane carry bugs.
        for input in [(1, 1), (2, 3), (3, 2), (5, 7), (8, 6)] {
            let source: Vec<_> = (0..input.0 * input.1)
                .flat_map(|pixel| {
                    [
                        (pixel * 31 % 47 + 20) as u8,
                        (pixel * 13 % 61 + 100) as u8,
                        (pixel * 7 % 31 + 200) as u8,
                        255,
                    ]
                })
                .collect();
            for width in 1..=9 {
                for height in 1..=9 {
                    let output = (width, height);
                    let plan = RecordingResizePlan::new(input.0, input.1, width, height);
                    let mut actual = vec![0; (width * height * 4) as usize];
                    plan.resize_into(&source, &mut actual);
                    assert_eq!(
                        actual,
                        area_reference(&source, input, output),
                        "cell coverage {input:?}->{output:?}"
                    );
                    for pixel in actual.chunks_exact(4) {
                        assert!((20..=66).contains(&pixel[0]));
                        assert!((100..=160).contains(&pixel[1]));
                        assert!((200..=230).contains(&pixel[2]));
                        assert_eq!(pixel[3], 255);
                    }
                }
            }
        }
    }

    fn available_narrow_kernels() -> Vec<NarrowKernels> {
        #[cfg(target_arch = "x86_64")]
        let sse41 = std::arch::is_x86_feature_detected!("sse4.1").then(|| {
            // Exercise SSE4.1 separately even when dispatch selects AVX2.
            NarrowKernels {
                horizontal: horizontal_sse41,
                accumulate: accumulate_sse41,
                write: write_sse41,
            }
        });
        #[cfg(not(target_arch = "x86_64"))]
        let sse41 = None;
        [NarrowKernels::portable(), NarrowKernels::new()]
            .into_iter()
            .chain(sse41)
            .collect()
    }

    #[cfg(target_arch = "aarch64")]
    #[test]
    fn arm64_selects_neon_area_kernels_and_processes_half_vectors() {
        let kernels = NarrowKernels::new();
        assert!(std::ptr::fn_addr_eq(
            kernels.horizontal,
            horizontal_neon as HorizontalNarrowKernel
        ));
        assert!(std::ptr::fn_addr_eq(
            kernels.accumulate,
            accumulate_neon as AccumulateNarrowKernel
        ));
        assert!(std::ptr::fn_addr_eq(
            kernels.write,
            write_neon as WriteNarrowKernel
        ));
        for offset in 0..16 {
            let top: Vec<_> = (0..160).map(|i| (i * 37 % 256) as u8).collect();
            let bottom: Vec<_> = (0..160).map(|i| (i * 71 % 256) as u8).collect();
            for start in 0..4 {
                for count in 0..=12 - start {
                    let range = start..start + count;
                    let mut actual = [0xa5; 64];
                    let mut expected = actual;
                    let top = &top[offset..offset + 96];
                    let bottom = &bottom[offset..offset + 96];
                    // Directly exercise the vector entry point and prove it
                    // processed the full batches, leaving the tail untouched.
                    let end =
                        unsafe { resize_half_neon(top, bottom, range.clone(), &mut actual[1..49]) };
                    assert_eq!(end, start + count / 4 * 4);
                    for x in start..end {
                        for c in 0..4 {
                            let sum = u16::from(top[x * 8 + c])
                                + u16::from(top[x * 8 + 4 + c])
                                + u16::from(bottom[x * 8 + c])
                                + u16::from(bottom[x * 8 + 4 + c]);
                            expected[1 + x * 4 + c] = ((sum + 2) / 4) as u8;
                        }
                    }
                    assert_eq!(actual, expected);
                }
            }
        }
    }

    #[test]
    #[ignore = "run explicitly in the performance Release build"]
    fn fractional_area_kernel_performance_comparison() {
        use std::hint::black_box;
        use std::time::{Duration, Instant};
        if cfg!(debug_assertions) {
            panic!("use the performance Release build");
        }
        #[allow(unused_mut)]
        let mut kernels = vec![("portable", NarrowKernels::portable())];
        #[cfg(target_arch = "aarch64")]
        kernels.push(("neon", NarrowKernels::new()));
        #[cfg(target_arch = "x86_64")]
        if std::arch::is_x86_feature_detected!("sse4.1") {
            kernels.push((
                "sse41",
                NarrowKernels {
                    horizontal: horizontal_sse41,
                    accumulate: accumulate_sse41,
                    write: write_sse41,
                },
            ));
            if std::arch::is_x86_feature_detected!("avx2") {
                kernels.push((
                    "avx2",
                    NarrowKernels {
                        horizontal: horizontal_sse41,
                        accumulate: accumulate_avx2,
                        write: write_avx2,
                    },
                ));
            }
        }
        println!(
            "pair,kernel,source_width,source_height,width,height,iterations,nanoseconds_per_resize"
        );
        for (input, output) in [
            ((2560, 1440), (1920, 1080)),
            ((2561, 1441), (1920, 1080)),
            ((1919, 1079), (959, 539)),
        ] {
            let source: Vec<_> = (0..input.0 * input.1 * 4)
                .map(|index| (index * 37 % 251) as u8)
                .collect();
            let plan = RecordingResizePlan::new(input.0, input.1, output.0, output.1);
            let area = plan.area.as_ref().unwrap();
            let reciprocal = area.average.reciprocal32.unwrap();
            let mut expected = vec![0; rgba_len(output)];
            plan.resize_into(&source, &mut expected);
            let mut pixels = vec![0; expected.len()];
            let run = |pixels: &mut [u8], kernels| {
                area.with_workspaces(1, |workspaces| {
                    area.resize_rows_narrow(
                        black_box(&source),
                        0,
                        0..output.0 as usize,
                        black_box(pixels),
                        &mut workspaces[0],
                        reciprocal,
                        kernels,
                    );
                });
            };
            // Warm storage and prove that every measured variant produces the
            // same pixels before taking paired timing observations.
            for (_, kernel) in &kernels {
                run(&mut pixels, *kernel);
                assert_eq!(pixels, expected);
            }
            for pair in 0..5 {
                for position in 0..kernels.len() {
                    let index = if pair % 2 == 0 {
                        position
                    } else {
                        kernels.len() - position - 1
                    };
                    let (name, kernel) = kernels[index];
                    let started = Instant::now();
                    let mut iterations = 0;
                    while started.elapsed() < Duration::from_millis(300) {
                        run(&mut pixels, kernel);
                        black_box(&pixels);
                        iterations += 1;
                    }
                    println!(
                        "{},{name},{},{},{},{},{iterations},{}",
                        pair + 1,
                        input.0,
                        input.1,
                        output.0,
                        output.1,
                        started.elapsed().as_nanos() / iterations
                    );
                }
            }
        }
    }

    #[test]
    fn concurrent_cloned_plans_never_reuse_another_frames_horizontal_pixels() {
        let plan = RecordingResizePlan::new(17, 13, 11, 7);
        let barrier = std::sync::Barrier::new(4);
        std::thread::scope(|scope| {
            for frame in 0..4 {
                let plan = plan.clone();
                let barrier = &barrier;
                scope.spawn(move || {
                    let source: Vec<_> = (0..17 * 13 * 4)
                        .map(|index| ((index * 37 + frame * 89) % 256) as u8)
                        .collect();
                    let expected = area_reference(&source, (17, 13), (11, 7));
                    barrier.wait();
                    for _ in 0..8 {
                        let mut actual = vec![0; expected.len()];
                        plan.resize_into(&source, &mut actual);
                        assert_eq!(actual, expected);
                    }
                });
            }
        });
    }

    #[test]
    fn narrow_rounding_matches_division_including_unsigned_lane_boundaries() {
        let kernels = available_narrow_kernels();
        for denominator in (2..=64).chain([
            255,
            256,
            257,
            959 * 539,
            2561 * 1441,
            1 << 24,
            u32::MAX / 256,
        ]) {
            let reciprocal = (1_u64 << 32).div_ceil(u64::from(denominator)) as u32;
            let maximum = 255 * denominator;
            for value in 0_u32..=255 {
                for remainder in [
                    0,
                    denominator / 2 - 1,
                    denominator / 2,
                    denominator / 2 + 1,
                    denominator - 1,
                ] {
                    let sum = (u64::from(value) * u64::from(denominator) + u64::from(remainder))
                        .min(u64::from(maximum)) as u32;
                    let source = [
                        [sum, maximum - sum, sum / 2, maximum],
                        [maximum, sum / 3, maximum - sum, sum],
                        [sum / 4, maximum - sum, sum, maximum],
                    ];
                    let expected: Vec<_> = source
                        .iter()
                        .flatten()
                        .map(|&sum| {
                            ((u64::from(sum) + u64::from(denominator / 2)) / u64::from(denominator))
                                as u8
                        })
                        .collect();
                    for kernels in &kernels {
                        let mut actual = [0; 12];
                        // SAFETY: feature-selected kernels receive a complete
                        // pixels and bounded numerator/correction products.
                        unsafe { (kernels.write)(&source, &mut actual, denominator, reciprocal) };
                        assert_eq!(
                            actual.as_slice(),
                            expected,
                            "{source:?}, denominator {denominator}"
                        );
                    }
                }
            }
        }
    }

    fn grayscale(values: &[u8]) -> Vec<u8> {
        values
            .iter()
            .flat_map(|&value| [value, value, value, 255])
            .collect()
    }

    #[test]
    fn downsampling_retains_thin_strokes_at_both_source_parities() {
        for vertical in [true, false] {
            for position in 0..4 {
                let mut source = grayscale(&[255; 16]);
                for index in 0..4 {
                    let pixel = if vertical {
                        index * 4 + position
                    } else {
                        position * 4 + index
                    };
                    source[pixel * 4..pixel * 4 + 3].fill(0);
                }
                let mut output = vec![0; 16];
                RecordingResizePlan::new(4, 4, 2, 2).resize_into(&source, &mut output);
                let values: Vec<_> = output.chunks_exact(4).map(|pixel| pixel[0]).collect();
                assert_eq!(values.iter().filter(|&&value| value == 128).count(), 2);
                assert_eq!(values.iter().filter(|&&value| value == 255).count(), 2);
                assert!(output.chunks_exact(4).all(|pixel| pixel[3] == 255));
                if position % 2 == 1 {
                    let mut nearest = vec![0; 16];
                    NearestResizePlan::new(4, 4, 2, 2).resize_into(&source, &mut nearest);
                    assert!(nearest.chunks_exact(4).all(|pixel| pixel[0] == 255));
                }
            }
        }
    }

    #[test]
    fn fractional_portrait_and_mixed_scaling_preserve_pixel_area() {
        for (source_size, output_size, values, expected) in [
            ((3, 1), (2, 1), vec![0, 90, 240], vec![30, 190]),
            ((1, 3), (1, 2), vec![0, 90, 240], vec![30, 190]),
            (
                (3, 1),
                (2, 3),
                vec![0, 90, 240],
                vec![30, 190, 30, 190, 30, 190],
            ),
            (
                (1, 3),
                (3, 2),
                vec![0, 90, 240],
                vec![30, 30, 30, 190, 190, 190],
            ),
            (
                (3, 3),
                (2, 2),
                vec![0, 30, 60, 90, 120, 150, 180, 210, 240],
                vec![40, 80, 160, 200],
            ),
        ] {
            let plan = RecordingResizePlan::new(
                source_size.0,
                source_size.1,
                output_size.0,
                output_size.1,
            );
            let mut output = vec![0; expected.len() * 4];
            plan.resize_into(&grayscale(&values), &mut output);
            assert_eq!(output, grayscale(&expected));
        }
    }

    #[test]
    fn native_pixels_constants_and_nearest_upscaling_are_preserved() {
        for (source_size, output_size) in [
            ((17, 9), (8, 4)),
            ((9, 17), (4, 8)),
            ((16, 8), (8, 4)),
            ((3, 7), (8, 4)),
            ((8, 4), (8, 4)),
            ((3, 5), (7, 11)),
        ] {
            let plan = RecordingResizePlan::new(
                source_size.0,
                source_size.1,
                output_size.0,
                output_size.1,
            );
            let source: Vec<_> = [12, 57, 203, 149]
                .into_iter()
                .cycle()
                .take((source_size.0 * source_size.1 * 4) as usize)
                .collect();
            let mut output = vec![0; (output_size.0 * output_size.1 * 4) as usize];
            let pointer = output.as_ptr();
            plan.resize_into(&source, &mut output);
            assert!(
                output
                    .chunks_exact(4)
                    .all(|pixel| pixel == [12, 57, 203, 149])
            );
            assert_eq!(output.as_ptr(), pointer);
            assert!(plan.matches(source_size, output_size));
            assert!(!plan.matches((source_size.0 + 1, source_size.1), output_size));
            if output_size.0 >= source_size.0 && output_size.1 >= source_size.1 {
                let source: Vec<_> = (0..source.len()).map(|index| (index % 251) as u8).collect();
                let mut nearest = vec![0; output.len()];
                NearestResizePlan::new(source_size.0, source_size.1, output_size.0, output_size.1)
                    .resize_into(&source, &mut nearest);
                plan.resize_into(&source, &mut output);
                assert_eq!(output, nearest);
            }
        }
    }

    #[test]
    fn area_half_specialization_matches_general_filter_for_all_row_tails() {
        for width in 1..=65 {
            let source: Vec<_> = (0..width * 2 * 6 * 4 + 1)
                .map(|index| (index * 37 % 251) as u8)
                .collect();
            // Exact 2:1 cases include every destination width and unaligned rows.
            let mut plan = RecordingResizePlan::new(width * 2, 6, width, 3);
            let mut specialized = vec![0; (width * 3 * 4) as usize];
            let mut general = vec![0; specialized.len()];
            plan.resize_into(&source[1..], &mut specialized);
            plan.set_bench_half_specialization(false);
            plan.resize_into(&source[1..], &mut general);
            assert_eq!(specialized, general);
        }
    }

    #[test]
    fn area_reciprocal_matches_division_at_rounding_boundaries_and_extreme_weights() {
        let denominators = (2..=1024).chain([
            8_294_400,
            8_293_399,
            1 << 32,
            (1 << 32) - 1,
            u64::MAX / 256 - 1,
            u64::MAX / 256,
        ]);
        for denominator in denominators {
            let average = AreaAverage::new(denominator);
            let maximum = 255 * denominator;
            for value in 0..=255 {
                for remainder in [
                    0,
                    denominator / 2 - 1,
                    denominator / 2,
                    denominator / 2 + 1,
                    denominator - 1,
                ] {
                    let sum = (value * denominator + remainder).min(maximum);
                    let reference = ((sum + denominator / 2) / denominator) as u8;
                    assert_eq!(
                        average.round(sum),
                        reference,
                        "sum {sum}, divisor {denominator}"
                    );
                }
            }
            let mut random = denominator;
            for _ in 0..1024 {
                random = random.wrapping_mul(6364136223846793005).wrapping_add(1);
                let sum = random % (maximum + 1);
                assert_eq!(
                    average.round(sum),
                    ((sum + denominator / 2) / denominator) as u8
                );
            }
        }
    }

    #[test]
    fn half_area_damage_handles_unaligned_vector_starts_and_scalar_tails() {
        let plan = RecordingResizePlan::new(34, 10, 17, 5);
        let mut source: Vec<_> = (0..34 * 10 * 4 + 1)
            .map(|index| (index * 37 % 251) as u8)
            .collect();
        let mut partial = vec![0; 17 * 5 * 4];
        plan.resize_into(&source[1..], &mut partial);
        for y in 1..9 {
            for x in 2..32 {
                source[1 + (y * 34 + x) * 4..1 + (y * 34 + x) * 4 + 4].fill(143);
            }
        }
        plan.resize_source_rect_into(&source[1..], &mut partial, (2, 1, 30, 8));
        let mut full = vec![0; partial.len()];
        plan.resize_into(&source[1..], &mut full);
        assert_eq!(partial, full);
    }

    #[test]
    fn damage_recomputes_every_intersecting_filter_footprint() {
        for (sw, sh, dw, dh) in [
            (17, 9, 8, 4),
            (9, 17, 4, 8),
            (8, 4, 8, 4),
            (3, 5, 7, 11),
            (16, 8, 8, 4),
            (3, 7, 8, 4),
            (7, 3, 4, 8),
            (9, 7, 1, 1),
        ] {
            let plan = RecordingResizePlan::new(sw, sh, dw, dh);
            let mut source: Vec<_> = (0..sw * sh * 4).map(|value| (value % 251) as u8).collect();
            let mut partial = vec![0; (dw * dh * 4) as usize];
            plan.resize_into(&source, &mut partial);
            for y in 0..sh {
                for x in 0..sw {
                    source[(y * sw + x) as usize * 4..(y * sw + x) as usize * 4 + 4].fill(255);
                    plan.resize_source_rect_into(&source, &mut partial, (x, y, 1, 1));
                    let mut full = vec![0; partial.len()];
                    plan.resize_into(&source, &mut full);
                    assert_eq!(
                        partial, full,
                        "source {sw}x{sh}, output {dw}x{dh}, damage {x},{y}"
                    );
                }
            }
            let previous = partial.clone();
            for rect in [(sw, sh, 1, 1), (0, 0, 0, sh), (0, 0, sw, 0)] {
                plan.resize_source_rect_into(&source, &mut partial, rect);
                assert_eq!(partial, previous);
            }
        }
    }

    #[test]
    fn bounded_workers_match_serial_filtering_and_fallback_geometries() {
        let pool = rayon::ThreadPoolBuilder::new()
            .num_threads(2)
            .build()
            .unwrap();
        for (sw, sh, dw, dh) in [
            (3840, 2160, 1920, 1080),
            (2160, 3840, 1080, 1920),
            (2561, 1441, 1920, 1080),
            (1920, 1080, 1920, 1080),
            (1919, 1079, 959, 539),
        ] {
            let source: Vec<_> = (0..sw * sh * 4).map(|value| (value % 251) as u8).collect();
            let plan = RecordingResizePlan::new(sw, sh, dw, dh);
            let mut serial = vec![0; (dw * dh * 4) as usize];
            let mut parallel = vec![165; serial.len()];
            plan.resize_into(&source, &mut serial);
            plan.resize_into_with_pool(&source, &mut parallel, &pool);
            assert_eq!(parallel, serial);
        }
    }

    #[test]
    #[should_panic]
    fn short_source_is_rejected_before_filter_access() {
        RecordingResizePlan::new(4, 4, 2, 2).resize_into(&[0; 4], &mut [0; 16]);
    }

    #[test]
    #[should_panic]
    fn wrong_destination_is_rejected_before_filter_access() {
        RecordingResizePlan::new(4, 4, 2, 2).resize_into(&[0; 64], &mut [0; 12]);
    }
}
