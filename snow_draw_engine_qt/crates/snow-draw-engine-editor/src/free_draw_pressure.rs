use snow_draw_engine_core::{Point, PressureKnot, pressure_radius};

#[derive(Clone, Copy, Debug, PartialEq)]
struct Sample {
    distance: f64,
    pressure: f64,
}

/// Scalar side channel. Position filtering/RDP never consults pressure, including
/// when the regular brush captures a profile for a later mode edit.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct PressureCapture {
    samples: Vec<Sample>,
    pub distance: f64,
    pub previous_distance: f64,
    last_point: Point<f64>,
    last_timestamp: u64,
    pub pressure: f64,
    simulation_scale: f64,
    sample_spacing: f64,
    next_sample_distance: f64,
}

impl PressureCapture {
    pub fn new(
        point: Point<f64>,
        pressure: Option<f64>,
        timestamp: u64,
        width: f64,
        sample_spacing: f64,
    ) -> Self {
        let pressure = pressure
            .filter(|p| p.is_finite())
            .unwrap_or(0.5)
            .clamp(0.0, 1.0);
        Self {
            samples: vec![Sample {
                distance: 0.0,
                pressure,
            }],
            distance: 0.0,
            previous_distance: 0.0,
            last_point: point,
            last_timestamp: timestamp,
            pressure,
            // perfect-freehand normalizes motion by its size before pressure easing.
            // Recover that size from Snow's nominal radius. Excalidraw's 4.25 factor
            // converts its UI width; Snow already stores the rendered width in pixels.
            simulation_scale: (pressure_radius(width, 0.5) / std::f64::consts::FRAC_1_SQRT_2)
                .max(1e-6),
            sample_spacing,
            next_sample_distance: sample_spacing,
        }
    }
    pub fn observe(&mut self, point: Point<f64>, pressure: Option<f64>, timestamp: u64) -> bool {
        if !point.x.is_finite() || !point.y.is_finite() {
            return false;
        }
        let step = (point.x - self.last_point.x).hypot(point.y - self.last_point.y);
        self.previous_distance = self.distance;
        self.distance += step;
        let previous = self.pressure;
        let native = pressure
            .filter(|p| p.is_finite())
            .map(|p| p.clamp(0.0, 1.0));
        let dt = if self.last_timestamp != 0 && timestamp > self.last_timestamp {
            (timestamp - self.last_timestamp) as f64
        } else {
            0.0
        };
        let speed = if dt > 0.0 {
            (step / dt * 16.0 / self.simulation_scale).clamp(0.0, 1.0)
        } else {
            0.0
        };
        // Adapted from perfect-freehand 1.2.0's simulated-pressure response and
        // RATE_OF_PRESSURE_CHANGE=0.275. A continuous-time exponential preserves
        // that response while making equal-velocity event subdivision invariant.
        // https://github.com/steveruizok/perfect-freehand/blob/v1.2.0/packages/perfect-freehand/src/getStrokeOutlinePoints.ts
        let target = 1.0 - speed;
        let decay = (-0.275 * speed).ln_1p() / 16.0;
        let value = |fraction: f64| {
            if let Some(native) = native {
                previous + (native - previous) * fraction
            } else if dt > 0.0 {
                target + (previous - target) * (decay * dt * fraction).exp()
            } else {
                previous
            }
        };
        self.pressure = native.unwrap_or_else(|| value(1.0));
        // Evaluate the continuous recurrence at fixed source-distance samples.
        // Subdividing a constant-velocity input segment produces identical profiles.
        if step > 1e-12 {
            while self.next_sample_distance <= self.distance + 1e-12 {
                let fraction =
                    ((self.next_sample_distance - self.previous_distance) / step).clamp(0.0, 1.0);
                self.push(Sample {
                    distance: self.next_sample_distance,
                    pressure: value(fraction),
                });
                self.next_sample_distance += self.sample_spacing;
            }
        }
        // Real pressure extrema can occur between spatial samples; retain those
        // event endpoints independently of the position filter.
        if native.is_some() {
            self.push(Sample {
                distance: self.distance,
                pressure: self.pressure,
            });
        }
        self.last_point = point;
        if timestamp > self.last_timestamp {
            self.last_timestamp = timestamp;
        }
        (previous - self.pressure).abs() > 1e-6
    }
    fn push(&mut self, sample: Sample) {
        if self.samples.last().is_some_and(|last| {
            last.distance == sample.distance && (last.pressure - sample.pressure).abs() < 1e-12
        }) {
            self.samples.last_mut().unwrap().pressure = sample.pressure;
            return;
        }
        if let Some(last) = self.samples.last()
            && last.distance == sample.distance
        {
            let start = self
                .samples
                .partition_point(|s| s.distance < sample.distance);
            let incoming = self.samples[start];
            let peak = self.samples[start..]
                .iter()
                .map(|s| s.pressure)
                .fold(sample.pressure, f64::max);
            self.samples.truncate(start);
            self.samples.push(incoming);
            if peak != incoming.pressure {
                self.samples.push(Sample {
                    pressure: peak,
                    ..sample
                });
            }
            if sample.pressure != self.samples.last().unwrap().pressure {
                self.samples.push(sample);
            }
            return;
        }
        // Online scalar simplification is independent of the centerline decision.
        if self.samples.len() >= 2 {
            let a = self.samples[self.samples.len() - 2];
            let b = self.samples[self.samples.len() - 1];
            let span = sample.distance - a.distance;
            if a.distance < b.distance && b.distance < sample.distance && span > 0.0 {
                let expected =
                    a.pressure + (sample.pressure - a.pressure) * (b.distance - a.distance) / span;
                if (expected - b.pressure).abs() < 0.005 {
                    self.samples.pop();
                }
            }
        }
        self.samples.push(sample);
        // Pathological subpixel motion must not grow the uncommitted scalar
        // window forever. Remove the lowest-prominence interior sample; peaks
        // win ties, and the endpoint and frozen-boundary anchor remain intact.
        const MAX_SCALAR_SAMPLES: usize = 4096;
        if self.samples.len() > MAX_SCALAR_SAMPLES {
            let index = (1..self.samples.len() - 1)
                .min_by(|&left, &right| {
                    let prominence = |index: usize| {
                        let a = self.samples[index - 1];
                        let b = self.samples[index];
                        let c = self.samples[index + 1];
                        let span = c.distance - a.distance;
                        let pressure = if span <= 1e-12 {
                            a.pressure.max(c.pressure)
                        } else {
                            a.pressure
                                + (c.pressure - a.pressure) * (b.distance - a.distance) / span
                        };
                        (b.pressure - pressure).abs()
                            + if b.pressure > a.pressure.max(c.pressure) {
                                1.0
                            } else {
                                0.0
                            }
                    };
                    prominence(left).total_cmp(&prominence(right))
                })
                .unwrap();
            self.samples.remove(index);
        }
    }
    fn at(&self, distance: f64) -> f64 {
        let after = self.samples.partition_point(|s| s.distance <= distance);
        if after == 0 {
            return self.samples[0].pressure;
        }
        let left = self.samples[after - 1];
        if after == self.samples.len() {
            let span = self.distance - left.distance;
            return if span <= 1e-12 {
                self.pressure
            } else {
                left.pressure
                    + (self.pressure - left.pressure)
                        * ((distance - left.distance) / span).clamp(0.0, 1.0)
            };
        }
        let right = self.samples[after];
        left.pressure
            + (right.pressure - left.pressure)
                * ((distance - left.distance) / (right.distance - left.distance)).clamp(0.0, 1.0)
    }
    pub fn profile(&self, begin: f64, end: f64) -> Vec<PressureKnot> {
        let span = end - begin;
        if span <= 1e-12 {
            let mut profile: Vec<_> = self
                .samples
                .iter()
                .filter(|s| s.distance == end)
                .map(|s| PressureKnot {
                    t: 0.0,
                    pressure: s.pressure,
                })
                .collect();
            if profile.is_empty() {
                profile.push(PressureKnot {
                    t: 0.0,
                    pressure: self.at(end),
                });
            }
            let pressure = profile.last().unwrap().pressure;
            profile.push(PressureKnot { t: 1.0, pressure });
            return profile;
        }
        let mut result: Vec<_> = self
            .samples
            .iter()
            .filter(|s| s.distance == begin)
            .map(|s| PressureKnot {
                t: 0.0,
                pressure: s.pressure,
            })
            .collect();
        if result.is_empty() {
            result.push(PressureKnot {
                t: 0.0,
                pressure: self.at(begin),
            });
        }
        result.extend(
            self.samples
                .iter()
                .filter(|s| s.distance > begin && s.distance <= end)
                .map(|s| PressureKnot {
                    t: ((s.distance - begin) / span).clamp(0.0, 1.0),
                    pressure: s.pressure,
                }),
        );
        if result.last().is_none_or(|knot| knot.t < 1.0) {
            result.push(PressureKnot {
                t: 1.0,
                pressure: self.at(end),
            });
        }
        result
    }
    pub fn discard_before(&mut self, distance: f64) {
        let keep = self
            .samples
            .partition_point(|s| s.distance < distance)
            .saturating_sub(1);
        self.samples.drain(..keep);
    }
    pub fn restart(&mut self, point: Point<f64>, distance: f64, pressure: f64) {
        self.distance = distance;
        self.previous_distance = distance;
        self.last_point = point;
        self.pressure = pressure;
        self.samples = vec![Sample { distance, pressure }];
        self.next_sample_distance = distance + self.sample_spacing;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::pressure_at;

    #[test]
    fn pressure_mouse_speed_varies_rendered_width_at_every_brush_size() {
        for width in [2.0, 4.0, 8.0, 16.0, 72.0] {
            for zoom in [0.25, 1.0, 4.0] {
                let simulated_radius = |step: f64| {
                    let mut capture =
                        PressureCapture::new(Point::default(), None, 100, width, 0.75 / zoom);
                    for i in 1..=64 {
                        capture.observe(Point::new(step * i as f64, 0.0), None, 100 + i * 16);
                    }
                    let profile = capture.profile(0.0, capture.distance);
                    pressure_radius(width, pressure_at(&profile, 0.75))
                };
                let slow = simulated_radius(width / 8.0);
                let fast = simulated_radius(width);
                assert!(
                    slow > fast * 2.0 && fast < (width * 0.5) * 0.75,
                    "width {width}, zoom {zoom}: slow radius {slow}, fast radius {fast}"
                );
            }
        }
    }

    #[test]
    fn pressure_native_sub_spacing_motion_retains_pressure_peak() {
        let mut capture = PressureCapture::new(Point::default(), Some(0.1), 1, 72.0, 0.75);
        capture.observe(Point::new(0.1, 0.0), Some(0.95), 9);
        capture.observe(Point::new(0.2, 0.0), Some(0.1), 17);
        capture.observe(Point::new(10.0, 0.0), Some(0.1), 25);
        let profile = capture.profile(0.0, 10.0);
        assert!(profile.iter().any(|knot| knot.pressure == 0.95));
    }

    #[test]
    fn pressure_mouse_recurrence_is_independent_of_event_density() {
        fn profile(subdivisions: usize) -> Vec<PressureKnot> {
            let mut capture = PressureCapture::new(Point::default(), None, 100, 12.0, 0.75);
            for i in 1..=subdivisions {
                capture.observe(
                    Point::new(160.0 * i as f64 / subdivisions as f64, 0.0),
                    None,
                    100 + 320 * i as u64 / subdivisions as u64,
                );
            }
            capture.profile(0.0, 160.0)
        }
        let sparse = profile(1);
        for count in [2, 8, 32] {
            let dense = profile(count);
            for i in 0..=100 {
                let t = i as f64 / 100.0;
                assert!(
                    (pressure_at(&sparse, t) - pressure_at(&dense, t)).abs() < 1e-12,
                    "subdivision {count}, t {t}: {:?} {:?}",
                    sparse,
                    dense
                );
            }
        }
    }
    #[test]
    fn pressure_missing_or_nonincreasing_timestamps_preserve_mouse_pressure() {
        let mut capture = PressureCapture::new(Point::default(), None, 100, 12.0, 0.75);
        for timestamp in [0, 90, 100] {
            capture.observe(Point::new(capture.distance + 10.0, 0.0), None, timestamp);
            assert_eq!(capture.pressure, 0.5);
        }
    }
    #[test]
    fn pressure_stationary_holds_keep_peak_and_bounded_memory() {
        let mut capture = PressureCapture::new(Point::default(), Some(0.2), 100, 12.0, 0.75);
        for i in 0..10000 {
            capture.observe(
                Point::default(),
                Some(if i % 2 == 0 { 0.9 } else { 0.3 }),
                101 + i,
            );
            assert!(capture.samples.len() <= 3);
        }
        let profile = capture.profile(0.0, 0.0);
        assert!(profile.iter().any(|knot| knot.pressure == 0.9));
        assert_eq!(profile.last().unwrap().pressure, 0.3);
    }
}
