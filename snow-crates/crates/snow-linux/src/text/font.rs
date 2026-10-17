//! Family-to-file font resolution for the X11 text path.
//!
//! The Windows and macOS adapters name a family and let `DirectWrite` /
//! `CoreText` find the file. An X11 client has no such service: the standard
//! answer is fontconfig, but fontconfig is a C library, and linking it would
//! cost the cross-target type check that the rest of this crate is verified
//! with. So the faces are enumerated instead -- every installed font's `name`
//! table is parsed with `ttf-parser` and matched against the requested family.
//!
//! Everything here is pure Rust, and the directory list is a parameter rather
//! than a constant, so the search is exercised on the Windows development
//! machine against `test-support/fonts` instead of only on a real desktop.

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

/// A font file plus the index of the face inside it.
///
/// Collections (`.ttc`, `.otc`) pack several faces into one file, and the index
/// is what disambiguates them when the file is opened.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FaceSource {
    pub path: PathBuf,
    pub index: u32,
}

/// Metadata read from a face's `name` table and `OS/2` table.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FaceInfo {
    pub source: FaceSource,
    /// Typographic family when the font declares one, legacy family otherwise.
    pub family: String,
    /// Style name, kept for diagnostics rather than for matching.
    pub subfamily: String,
    /// `usWeightClass`, which is the OpenType weight on the 1..=1000 scale.
    pub weight: u32,
    pub italic: bool,
}

/// A family request from the application.
///
/// `family` may be empty, which asks for the first family in
/// [`DEFAULT_FAMILIES`] that is installed. `cjk_family` is only consulted for
/// characters the primary face cannot render.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FontRequest<'a> {
    pub family: &'a str,
    pub cjk_family: &'a str,
    pub weight: u32,
}

/// Families probed when the application names none.
///
/// Ordered by how likely a UOS or Deepin desktop is to have them: the Noto CJK
/// family is what both ship as the default UI font, and the rest are the
/// fallbacks a trimmed image still tends to keep.
pub const DEFAULT_FAMILIES: &[&str] = &[
    "Noto Sans CJK SC",
    "Source Han Sans SC",
    "WenQuanYi Micro Hei",
    "Noto Sans",
    "DejaVu Sans",
    "Liberation Sans",
];

/// Families probed for characters the primary face cannot render.
pub const DEFAULT_CJK_FAMILIES: &[&str] = &[
    "Noto Sans CJK SC",
    "Source Han Sans SC",
    "WenQuanYi Micro Hei",
    "Noto Sans CJK TC",
    "Noto Sans CJK JP",
];

const FONT_EXTENSIONS: [&str; 4] = ["ttf", "otf", "ttc", "otc"];
/// Depth guard for the directory walk. Font trees are shallow; this only stops
/// a symlink loop from turning one scan into an unbounded one.
const MAX_DEPTH: usize = 6;

/// Directories a Linux desktop keeps fonts in, in probe order.
///
/// Per-user directories come first so an application-installed font overrides
/// the packaged one, matching fontconfig's precedence. `/usr/share/fonts` is
/// where UOS and Deepin place both `truetype` and `opentype` subtrees.
#[cfg(target_os = "linux")]
pub fn system_font_dirs() -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    if let Some(home) = std::env::var_os("HOME") {
        let home = PathBuf::from(home);
        let data = std::env::var_os("XDG_DATA_HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|| home.join(".local/share"));
        dirs.push(data.join("fonts"));
        dirs.push(home.join(".fonts"));
    }
    dirs.push(PathBuf::from("/usr/local/share/fonts"));
    dirs.push(PathBuf::from("/usr/share/fonts"));
    dirs
}

/// No installed-font tree is defined for other targets: the text adapter is
/// Linux-only, and the enumeration tests point at an explicit fixture
/// directory instead of relying on this list.
#[cfg(not(target_os = "linux"))]
pub fn system_font_dirs() -> Vec<PathBuf> {
    Vec::new()
}

/// The faces installed on this machine, enumerated once per process.
pub fn installed_faces() -> &'static [FaceInfo] {
    static FACES: std::sync::OnceLock<Vec<FaceInfo>> = std::sync::OnceLock::new();
    FACES.get_or_init(|| scan_faces(&system_font_dirs()))
}

/// Read every face under `dirs`.
///
/// The walk is sorted before parsing, so the resulting order does not depend on
/// the filesystem's readdir order and a tie between two equal faces resolves
/// the same way on every run. Each file is read, parsed and dropped in turn: a
/// machine with several hundred installed fonts never holds more than one of
/// them in memory.
pub fn scan_faces(dirs: &[PathBuf]) -> Vec<FaceInfo> {
    let mut files = BTreeSet::new();
    for dir in dirs {
        collect_files(dir, 0, &mut files);
    }

    let mut faces = Vec::new();
    for path in files {
        let Ok(data) = std::fs::read(&path) else {
            // An unreadable font is not fatal: a desktop routinely keeps
            // root-only or broken files under its font tree.
            continue;
        };
        let count = ttf_parser::fonts_in_collection(&data).unwrap_or(1);
        for index in 0..count {
            if let Some(info) = read_face(&path, index, &data) {
                faces.push(info);
            }
        }
    }
    faces
}

fn collect_files(dir: &Path, depth: usize, out: &mut BTreeSet<PathBuf>) {
    if depth > MAX_DEPTH {
        return;
    }
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        // `file_type` does not follow symlinks, so a link to a parent directory
        // is skipped rather than walked forever.
        let Ok(kind) = entry.file_type() else {
            continue;
        };
        if kind.is_symlink() {
            continue;
        }
        if kind.is_dir() {
            collect_files(&path, depth + 1, out);
        } else if kind.is_file() && has_font_extension(&path) {
            out.insert(path);
        }
    }
}

fn has_font_extension(path: &Path) -> bool {
    path.extension()
        .and_then(|value| value.to_str())
        .map(|value| {
            let value = value.to_ascii_lowercase();
            FONT_EXTENSIONS.contains(&value.as_str())
        })
        .unwrap_or(false)
}

fn read_face(path: &Path, index: u32, data: &[u8]) -> Option<FaceInfo> {
    let face = ttf_parser::Face::parse(data, index).ok()?;
    let (family, subfamily) = read_names(&face)?;
    Some(FaceInfo {
        source: FaceSource {
            path: path.to_path_buf(),
            index,
        },
        family,
        subfamily,
        weight: u32::from(face.weight().to_number()),
        italic: face.is_italic(),
    })
}

/// Pull the family and style out of the `name` table.
///
/// Both spellings are considered: the typographic ids (16/17) are the
/// preferred ones and carry the family without a style suffix, while the
/// legacy ids (1/2) are what older fonts have. Record `0x0409` (US English) is
/// then preferred over any other localization, so the same face is identified
/// whatever the machine's locale is.
fn read_names(face: &ttf_parser::Face<'_>) -> Option<(String, String)> {
    use ttf_parser::name_id;

    let mut family: Option<(u8, String)> = None;
    let mut subfamily: Option<(u8, String)> = None;

    for name in face.names() {
        let is_family = name.name_id == name_id::TYPOGRAPHIC_FAMILY
            || name.name_id == name_id::FAMILY;
        let is_subfamily = name.name_id == name_id::TYPOGRAPHIC_SUBFAMILY
            || name.name_id == name_id::SUBFAMILY;
        if !is_family && !is_subfamily {
            continue;
        }
        let Some(value) = name.to_string() else {
            continue;
        };
        let value = value.trim().to_string();
        if value.is_empty() {
            continue;
        }
        // Lower score wins: typographic ids beat legacy ids, and the US English
        // record beats a localized one.
        let score = u8::from(name.name_id != name_id::TYPOGRAPHIC_FAMILY)
            + u8::from(name.name_id != name_id::TYPOGRAPHIC_SUBFAMILY)
            + 2 * u8::from(name.language_id != 0x0409);
        let slot = if is_family { &mut family } else { &mut subfamily };
        if slot.as_ref().is_none_or(|(best, _)| score < *best) {
            *slot = Some((score, value));
        }
    }

    Some((family?.1, subfamily.map(|(_, value)| value).unwrap_or_default()))
}

/// Fold a family name for comparison.
///
/// Case is insignificant, and so are spaces, hyphens and underscores: the same
/// family reaches this module as `Noto Sans CJK SC` from a Qt request and as
/// `Noto Sans CJK SC` or `Noto-Sans-CJK-SC` in different `name` tables.
fn normalize(name: &str) -> String {
    name.chars()
        .filter(|value| !value.is_whitespace() && *value != '-' && *value != '_')
        .map(|value| value.to_ascii_lowercase())
        .collect()
}

/// Pick the installed face closest to `family`/`weight`.
///
/// Upright faces win over italics at equal weight distance, and the sorted file
/// order breaks any remaining tie, so the choice is deterministic. `None` means
/// the family is not installed, which the caller reports rather than silently
/// substituting another typeface.
pub fn select_face(faces: &[FaceInfo], family: &str, weight: u32) -> Option<FaceSource> {
    let wanted = normalize(family);
    if wanted.is_empty() {
        return None;
    }

    let mut best: Option<(u32, bool, &FaceInfo)> = None;
    for face in faces {
        if normalize(&face.family) != wanted {
            continue;
        }
        let distance = face.weight.abs_diff(weight);
        let better = match best {
            None => true,
            Some((best_distance, best_italic, _)) => {
                (distance, face.italic) < (best_distance, best_italic)
            }
        };
        if better {
            best = Some((distance, face.italic, face));
        }
    }
    best.map(|(_, _, face)| face.source.clone())
}

/// Resolve a request into the primary face plus an optional fallback face.
///
/// The primary family is tried first; when the request names none, the defaults
/// are probed in order. A missing fallback is not an error: a machine with no
/// CJK font still renders Latin legends, and the caller only fails when a
/// character has no face at all.
pub fn resolve(request: &FontRequest<'_>, faces: &[FaceInfo]) -> Option<(FaceSource, Option<FaceSource>)> {
    let primary = if request.family.trim().is_empty() {
        DEFAULT_FAMILIES
            .iter()
            .find_map(|family| select_face(faces, family, request.weight))?
    } else {
        select_face(faces, request.family, request.weight)?
    };

    let cjk = if request.cjk_family.trim().is_empty() {
        DEFAULT_CJK_FAMILIES
            .iter()
            .find_map(|family| select_face(faces, family, request.weight))
    } else {
        select_face(faces, request.cjk_family, request.weight)
    };
    // A fallback identical to the primary adds a second copy of the same face
    // for no benefit.
    let cjk = cjk.filter(|source| *source != primary);
    Some((primary, cjk))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture_dir() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../test-support/fonts")
    }

    fn fixtures() -> Vec<FaceInfo> {
        scan_faces(&[fixture_dir()])
    }

    #[test]
    fn fixtures_expose_both_styles_of_every_family() {
        let faces = fixtures();
        for family in ["Snow Recording Test Sans", "Snow Recording Test Mono", "Snow Recording Test Han"] {
            let weights: Vec<u32> = faces
                .iter()
                .filter(|face| face.family == family)
                .map(|face| face.weight)
                .collect();
            assert_eq!(weights.len(), 2, "{family} should have two faces");
            assert!(weights.contains(&400), "{family} lacks regular");
            assert!(weights.contains(&700), "{family} lacks bold");
        }
    }

    #[test]
    fn scanning_is_deterministic() {
        // The same directory must enumerate in the same order every time, or a
        // weight tie would resolve differently between runs.
        assert_eq!(fixtures(), fixtures());
    }

    #[test]
    fn family_matching_ignores_case_and_separators() {
        let faces = fixtures();
        for spelling in [
            "Snow Recording Test Sans",
            "snow recording test sans",
            "Snow-Recording-Test-Sans",
            "SNOW_RECORDING_TEST SANS",
        ] {
            let face = select_face(&faces, spelling, 400).expect(spelling);
            assert_eq!(face, select_face(&faces, "Snow Recording Test Sans", 400).unwrap());
        }
    }

    #[test]
    fn the_closest_weight_wins() {
        let faces = fixtures();
        let regular = select_face(&faces, "Snow Recording Test Sans", 400).unwrap();
        let bold = select_face(&faces, "Snow Recording Test Sans", 700).unwrap();
        assert_ne!(regular, bold);
        // Qt weights are on the same 1..=1000 scale, so 500 must land on one of
        // the two installed faces rather than being rejected.
        let medium = select_face(&faces, "Snow Recording Test Sans", 500).unwrap();
        assert!(medium == regular || medium == bold);
        // Far outside the installed range still resolves.
        assert!(select_face(&faces, "Snow Recording Test Sans", 900).is_some());
    }

    #[test]
    fn an_installed_family_is_never_substituted() {
        let faces = fixtures();
        assert!(select_face(&faces, "Noto Sans CJK SC", 400).is_none());
        assert!(select_face(&faces, "", 400).is_none());
    }

    #[test]
    fn resolve_falls_back_when_the_request_names_nothing() {
        // The fixtures hold none of the default families, so a request with an
        // empty family must fail rather than pick an arbitrary face.
        let faces = fixtures();
        let request = FontRequest {
            family: "",
            cjk_family: "",
            weight: 400,
        };
        assert!(resolve(&request, &faces).is_none());

        let request = FontRequest {
            family: "Snow Recording Test Sans",
            cjk_family: "Snow Recording Test Han",
            weight: 400,
        };
        let (primary, cjk) = resolve(&request, &faces).unwrap();
        assert_ne!(primary, cjk.unwrap());
    }

    #[test]
    fn a_fallback_equal_to_the_primary_is_dropped() {
        let faces = fixtures();
        let request = FontRequest {
            family: "Snow Recording Test Sans",
            cjk_family: "Snow Recording Test Sans",
            weight: 400,
        };
        let (_, cjk) = resolve(&request, &faces).unwrap();
        assert_eq!(cjk, None);
    }

    #[test]
    fn a_missing_fallback_is_not_an_error() {
        // Latin legends must still render on a desktop with no CJK font.
        let faces = fixtures();
        let request = FontRequest {
            family: "Snow Recording Test Sans",
            cjk_family: "Noto Sans CJK SC",
            weight: 400,
        };
        let (primary, cjk) = resolve(&request, &faces).unwrap();
        assert_eq!(cjk, None);
        assert!(primary.path.ends_with("SnowRecordingTestSans-Regular.ttf"));
    }

    #[test]
    fn only_font_extensions_are_scanned() {
        assert!(has_font_extension(Path::new("/usr/share/fonts/a.ttf")));
        assert!(has_font_extension(Path::new("/usr/share/fonts/a.OTF")));
        assert!(has_font_extension(Path::new("/usr/share/fonts/a.ttc")));
        assert!(!has_font_extension(Path::new("/usr/share/fonts/fonts.dir")));
        assert!(!has_font_extension(Path::new("/usr/share/fonts/README")));
    }

    #[test]
    fn an_unreadable_directory_is_skipped() {
        let faces = scan_faces(&[PathBuf::from("/definitely/not/a/font/dir")]);
        assert!(faces.is_empty());
    }
}
