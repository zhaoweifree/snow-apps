//! Platform-independent language detection C ABI. See include/snow_language_detection.h.

use std::ffi::c_char;
use std::{slice, str};

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SnowLanguageDetectionResult {
    pub language: [c_char; 4],
    pub confidence: f64,
}

fn detect(text: &str) -> Option<SnowLanguageDetectionResult> {
    let info = whatlang::detect(text)?;
    let mut result = SnowLanguageDetectionResult {
        confidence: info.confidence(),
        ..Default::default()
    };
    for (destination, source) in result.language.iter_mut().zip(info.lang().code().bytes()) {
        *destination = source as c_char;
    }
    Some(result)
}

/// Detect the complete UTF-8 input with a single whatlang call.
///
/// Returns 1 for a detection, 0 for no detection, and -1 for invalid arguments or UTF-8.
/// A non-null output is cleared even when detection fails.
///
/// # Safety
/// Non-null input must reference `length` readable bytes for the duration of this call.
/// Non-null output must reference a writable, aligned result and must not overlap input.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_language_detect(
    input: *const u8,
    length: usize,
    output: *mut SnowLanguageDetectionResult,
) -> i32 {
    if output.is_null() {
        return -1;
    }
    unsafe { output.write(SnowLanguageDetectionResult::default()) };
    if length == 0 {
        return 0;
    }
    if input.is_null() || length > isize::MAX as usize {
        return -1;
    }
    let bytes = unsafe { slice::from_raw_parts(input, length) };
    let Ok(text) = str::from_utf8(bytes) else {
        return -1;
    };
    let Some(result) = detect(text) else {
        return 0;
    };
    unsafe { output.write(result) };
    1
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ptr;

    fn run(input: &[u8]) -> (i32, SnowLanguageDetectionResult) {
        let mut result = SnowLanguageDetectionResult {
            language: [1; 4],
            confidence: -1.0,
        };
        let status = unsafe { snow_language_detect(input.as_ptr(), input.len(), &mut result) };
        (status, result)
    }

    #[test]
    fn detects_complete_multiline_input_with_raw_confidence() {
        let input = "This document describes the application's translation settings.\n\
                     The primary target language determines whether the secondary target is used.\n\
                     Every line belongs to the same input and is analyzed together.";
        let expected = whatlang::detect(input).expect("English paragraph should be detected");
        let (status, result) = run(input.as_bytes());
        assert_eq!(status, 1);
        assert_eq!(
            result.language,
            [b'e' as c_char, b'n' as c_char, b'g' as c_char, 0]
        );
        assert_eq!(result.confidence, expected.confidence());
        assert!((0.0..=1.0).contains(&result.confidence));
    }

    #[test]
    fn reports_mandarin_without_distinguishing_chinese_scripts() {
        for input in [
            "这是一个中文语言检测测试。所有输入文字一起分析，只有置信度达到设置的阈值时才切换翻译目标语言。",
            "這是一個中文語言偵測測試。所有輸入文字一起分析，只有信心程度達到設定的門檻時才切換翻譯目標語言。",
        ] {
            let (status, result) = run(input.as_bytes());
            assert_eq!(status, 1);
            assert_eq!(
                result.language,
                [b'c' as c_char, b'm' as c_char, b'n' as c_char, 0]
            );
            assert!((0.0..=1.0).contains(&result.confidence));
        }
    }

    #[test]
    fn empty_or_nonlinguistic_input_has_no_detection_and_clears_output() {
        for input in ["", " \n\t", "1234567890 !@#$%^&*()"] {
            let (status, result) = run(input.as_bytes());
            assert_eq!(status, 0);
            assert_eq!(result, SnowLanguageDetectionResult::default());
        }
        let mut result = SnowLanguageDetectionResult::default();
        assert_eq!(
            unsafe { snow_language_detect(ptr::null(), 0, &mut result) },
            0
        );
    }

    #[test]
    fn invalid_arguments_or_utf8_are_rejected_and_clear_output() {
        let (status, result) = run(&[0xff, 0xfe]);
        assert_eq!(status, -1);
        assert_eq!(result, SnowLanguageDetectionResult::default());
        let mut result = SnowLanguageDetectionResult {
            language: [1; 4],
            confidence: -1.0,
        };
        assert_eq!(
            unsafe { snow_language_detect(ptr::null(), 1, &mut result) },
            -1
        );
        assert_eq!(result, SnowLanguageDetectionResult::default());
        let input = b"text";
        assert_eq!(
            unsafe { snow_language_detect(input.as_ptr(), input.len(), ptr::null_mut()) },
            -1
        );
        assert_eq!(
            unsafe { snow_language_detect(input.as_ptr(), usize::MAX, &mut result) },
            -1
        );
        assert_eq!(result, SnowLanguageDetectionResult::default());
    }
}
