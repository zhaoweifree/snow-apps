function Get-SnowOcrToolchain {
    $pin = Join-Path $PSScriptRoot '../snow-crates/crates/simd-paddle-ocr-rs/rust-toolchain.toml'
    if (-not (Test-Path -LiteralPath $pin)) { throw 'Initialize Git submodules before building OCR.' }
    $source = Get-Content -LiteralPath $pin -Raw
    if ($source -cnotmatch '(?m)^channel\s*=\s*"(nightly-\d{4}-\d{2}-\d{2})"') {
        throw 'Invalid OCR toolchain pin.'
    }
    return $Matches[1]
}
