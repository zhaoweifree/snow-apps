#!/usr/bin/env python3
"""Keeps the OpenCV options of `scripts/build-linux-deps.sh` honest.

Why this gate exists
--------------------
OpenCV 4.12 is configured with `ENABLE_CONFIG_VERIFICATION=ON` -- the same
switch the vcpkg overlay port uses. OpenCV then runs `ocv_verify_config()`
(cmake/OpenCVUtils.cmake) at the end of the configure: for every option
declared with a `VERIFY` clause it evaluates the dependency flag and
`FATAL_ERROR`s if the option and the flag disagree. Run 19 died there and
only printed `opencv installed no OpenCVConfig.cmake`, because the archive
assertion at the end of `build_opencv()` fires after cmake has already
refused to configure.

Two facts about `OCV_OPTION` decide what the script has to state:

  * the `VERIFY` registration sits *outside* the `if(${__condition})` block,
    so an option that is invisible on this platform is still added to
    `OPENCV_VERIFICATIONS` -- but it is never `option()`-declared, its value
    expands to nothing and neither branch of the verification fires.
    Invisible options are therefore harmless and must not be listed here;
  * every dependency probe is guarded by its own option. The videoio and
    highgui backends use `add_backend(<id> <OPTION>)` in their `cmake/init.cmake`,
    which `ocv_glob_modules()` includes for *every* module in the first pass
    -- a module outside `BUILD_LIST` still runs its detection. The guard is
    the option, not the module list.

So the rule is one-directional: an option that is OFF never fails the
verification (nothing is probed, the flag stays false), while an option that
is ON with a missing dependency always does. Only the options that default
to ON *and* cannot be satisfied by the CI image have to be stated as OFF.

Not every trap goes through the verification, though. `WITH_ADE` carries no
`VERIFY` clause and is caught one step later, by whoever calls
`find_package(OpenCV CONFIG)`: it makes OpenCV export an `ade` target that it
never installs. `REQUIRED_OFF_UNVERIFIED` covers that class -- options that
break the installed package directly, without any cmake error at build time.

`REQUIRED_OFF` is that set. It was not written by hand: it is the failure
list of a replication of the upstream `OCV_OPTION` macro and
`ocv_verify_config()` (extracted verbatim from OpenCV 4.12.0, evaluated with
cmake 4.2.3, a Linux/x86_64 platform variable model and dependency results
driven from the workflow's apt list). Fed with no options at all it names 15
options; fed with the option set of run 19 it names the 11 that were missing.
The sixteenth, `WITH_TESSERACT`, came out of run 20 instead: it is declared by
`opencv_contrib/modules/text/cmake/init.cmake`, whose default is
`(NOT CMAKE_CROSSCOMPILING)`, so it is ON for a native build -- the replication
missed it because it only read OpenCV's own root CMakeLists.txt.

The vcpkg port reaches the same state through its feature map, which passes
`-DWITH_<x>=OFF` for every feature that is not enabled. The difference is
that vcpkg owns the dependency set, while this script has to state it
against the packages the CI installs.

A second thing this gate catches: an option name OpenCV does not declare is
accepted by cmake and silently ignored, so a typo or a name removed upstream
turns the intended "off" back into the default "on". `-DWITH_ADE=OFF` was
such a case -- 4.12 has no `WITH_ADE` option at all.

Run it directly; the Linux packaging gate runs it as well.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

WORKFLOW = ROOT / ".github" / "workflows" / "snow-shot-linux.yml"
DEPS_SCRIPT = ROOT / "scripts" / "build-linux-deps.sh"
OVERLAY_PORT = ROOT / "cmake" / "vcpkg-overlay-ports" / "opencv4" / "portfile.cmake"

# Options that are visible on Linux, default to ON, and whose dependency the
# CI image does not provide. Value: where OpenCV looks for that dependency.
REQUIRED_OFF = {
    "WITH_AVIF": "cmake/OpenCVFindLibsGrfmt.cmake -> libavif (no libavif-dev)",
    "WITH_EIGEN": "cmake/OpenCVFindLibsPerf.cmake -> Eigen3 (no libeigen3-dev)",
    "WITH_GSTREAMER": "modules/videoio/cmake/detect_gstreamer.cmake -> "
                      "pkg-config gstreamer-1.0 (no libgstreamer1.0-dev)",
    "WITH_GTK": "modules/highgui/cmake/detect_gtk.cmake -> "
                "pkg-config gtk+-3.0 / gtk+-2.0 (no libgtk-3-dev)",
    "WITH_IPP": "cmake/OpenCVFindIPP.cmake -> ippicv archive (no system IPP)",
    "WITH_ITT": "cmake/OpenCVFindLibsPerf.cmake -> Intel ITT (not installed)",
    "WITH_JASPER": "cmake/OpenCVFindLibsGrfmt.cmake -> Jasper (no libjasper-dev)",
    "WITH_LAPACK": "cmake/OpenCVFindLAPACK.cmake -> LAPACK (no liblapack-dev)",
    "WITH_OPENCL": "cmake/OpenCVDetectOpenCL.cmake -> find_package(OpenCL) "
                   "(no ocl-icd-opencl-dev)",
    "WITH_OPENCLAMDBLAS": "cmake/OpenCVDetectOpenCL.cmake -> clAmdBlas (not installed)",
    "WITH_OPENCLAMDFFT": "cmake/OpenCVDetectOpenCL.cmake -> clAmdFft (not installed)",
    "WITH_OPENJPEG": "cmake/OpenCVFindLibsGrfmt.cmake -> OpenJPEG (no libopenjp2-7-dev)",
    "WITH_TESSERACT": "opencv_contrib/modules/text/cmake/init.cmake -> Tesseract "
                      "(no libtesseract-dev); default is (NOT CMAKE_CROSSCOMPILING), "
                      "so it is ON for a native build",
    "WITH_VA": "cmake/OpenCVFindVA.cmake -> va/va.h (no libva-dev)",
    "WITH_VA_INTEL": "cmake/OpenCVFindVA.cmake -> Intel VA-API/OpenCL (no libva-dev)",
    "WITH_VTK": "cmake/OpenCVDetectVTK.cmake -> find_package(VTK) (no libvtk9-dev)",
}

# Options that `ocv_verify_config()` cannot see -- they carry no `VERIFY`
# clause -- but that still break the installed package when they are on.
# Value: what goes wrong.
REQUIRED_OFF_UNVERIFIED = {
    "WITH_ADE": "modules/gapi/cmake/init.cmake (default ON) downloads ADE and "
                "exports an `ade` target from OpenCVModules.cmake; gapi is not in "
                "BUILD_LIST, so nothing builds or installs libade.a and every "
                "find_package(OpenCV CONFIG) aborts with 'The imported target "
                "\"ade\" references the file ... but this file does not exist' "
                "(run 21)",
}

# Every `OCV_OPTION(WITH_..., ...)` OpenCV 4.12.0 declares: the 90 in the root
# CMakeLists.txt, the three that opencv_contrib modules declare in their
# `cmake/init.cmake`, and the five that the gapi module declares in its own.
# Those files are *not* conditional on the module being built --
# `ocv_glob_modules()` includes every module's `cmake/init.cmake` in its first
# pass -- so a module option can end up deciding the fate of a build that only
# asks for `wechat_qrcode`. That is how both `WITH_TESSERACT` (verification
# failure) and `WITH_ADE` (broken imported target) got here.
# Used to reject options that do not exist, which cmake accepts silently.
DECLARED_WITH_OPTIONS = frozenset(
    """
    WITH_1394 WITH_ANDROID_MEDIANDK WITH_ANDROID_NATIVE_CAMERA WITH_ARAVIS
    WITH_AVFOUNDATION WITH_AVIF WITH_CANN WITH_CAP_IOS WITH_CAROTENE WITH_CLP
    WITH_CPUFEATURES WITH_CUBLAS WITH_CUDA WITH_CUDNN WITH_CUFFT WITH_DIRECTML
    WITH_DIRECTX WITH_DSHOW WITH_EIGEN WITH_FASTCV WITH_FFMPEG WITH_FLATBUFFERS
    WITH_FRAMEBUFFER WITH_FRAMEBUFFER_XVFB WITH_GDAL WITH_GDCM WITH_GPHOTO2
    WITH_GSTREAMER WITH_GTK WITH_GTK_2_X WITH_HALIDE WITH_HAL_RVV WITH_HPX
    WITH_IMGCODEC_GIF WITH_IMGCODEC_HDR WITH_IMGCODEC_PFM WITH_IMGCODEC_PXM
    WITH_IMGCODEC_SUNRASTER WITH_IPP WITH_ITT WITH_JASPER WITH_JPEG WITH_JPEGXL
    WITH_KLEIDICV WITH_LAPACK WITH_LIBREALSENSE WITH_MFX WITH_MSMF WITH_MSMF_DXVA
    WITH_NDSRVP WITH_NVCUVENC WITH_NVCUVID WITH_OBSENSOR WITH_ONNX WITH_OPENCL
    WITH_OPENCLAMDBLAS WITH_OPENCLAMDFFT WITH_OPENCL_D3D11_NV WITH_OPENCL_SVM
    WITH_OPENEXR WITH_OPENGL WITH_OPENJPEG WITH_OPENMP WITH_OPENNI WITH_OPENNI2
    WITH_OPENVINO WITH_OPENVX WITH_PNG WITH_PROTOBUF WITH_PTHREADS_PF WITH_PVAPI
    WITH_QT WITH_QUIRC WITH_SPNG WITH_TBB WITH_TESSERACT WITH_TIFF WITH_TIMVX
    WITH_UEYE WITH_V4L WITH_VA WITH_VA_INTEL WITH_VTK WITH_VULKAN WITH_WAYLAND
    WITH_WEBNN WITH_WEBP WITH_WIN32UI WITH_XIMEA WITH_XINE WITH_ZLIB_NG
    WITH_JULIA WITH_MATLAB
    WITH_ADE WITH_FREETYPE WITH_GAPI_ONEVPL WITH_OAK WITH_PLAIDML
    """.split()
)

_failures = 0


def fail(message: str) -> None:
    global _failures
    print(f"FAIL: {message}")
    _failures += 1


def workflow_apt_packages() -> set[str]:
    """Every package the workflow installs with apt-get."""
    text = WORKFLOW.read_text(encoding="utf-8")
    packages: set[str] = set()
    for block in re.findall(r"apt-get install[^\n]*\\\n((?:[^\n]*\\\n)*[^\n]*)", text):
        for token in block.replace("\\\n", " ").split():
            token = token.strip()
            if token and not token.startswith("-"):
                packages.add(token)
    return packages


def opencv_configure_text() -> str:
    """The `configure_build_install ...` call of build_opencv(), continuations joined."""
    text = DEPS_SCRIPT.read_text(encoding="utf-8")
    start = text.find("build_opencv()")
    if start < 0:
        fail("scripts/build-linux-deps.sh has no build_opencv() function")
        return ""
    end = text.find("\n}\n", start)
    function = text[start:end if end > 0 else len(text)]

    anchor = function.find("-DOPENCV_EXTRA_MODULES_PATH")
    if anchor < 0:
        fail("build_opencv() does not configure OpenCV with extra modules")
        return ""
    head = function.rfind("configure_build_install", 0, anchor)
    segment = function[head if head >= 0 else 0:]
    # The call ends at the line that closes it.
    close = re.search(r"\n\s*\)\s*\n", segment)
    if close:
        segment = segment[: close.end()]
    return segment.replace("\\\n", " ")


def opencv_options() -> dict[str, str]:
    """The `-DNAME=VALUE` options of the OpenCV configure call."""
    return dict(re.findall(r"-D([A-Za-z0-9_]+)=([A-Za-z0-9_.\-/]+)", opencv_configure_text()))


def main() -> int:
    for path in (WORKFLOW, DEPS_SCRIPT):
        if not path.is_file():
            print(f"FAIL: {path} is missing")
            return 1

    packages = workflow_apt_packages()
    if not packages:
        fail("the workflow's apt package list could not be read")
        return 1

    options = opencv_options()
    if not options:
        fail("the OpenCV configure options could not be read")
        return 1

    # The verification is a hard gate only when the script asks for it -- and
    # it has to ask for it exactly when the overlay port does, because that is
    # the "same source, same options" rule (port document 9.6.14).
    port_text = OVERLAY_PORT.read_text(encoding="utf-8") if OVERLAY_PORT.is_file() else ""
    port_verifies = "ENABLE_CONFIG_VERIFICATION=ON" in port_text
    script_verifies = options.get("ENABLE_CONFIG_VERIFICATION") == "ON"
    if port_verifies != script_verifies:
        fail(
            "the OpenCV configuration verification does not match the overlay port "
            f"(port: {port_verifies}, script: {script_verifies})"
        )

    # 1. Every option whose dependency the image cannot provide has to be OFF.
    if script_verifies:
        for option, probe in sorted(REQUIRED_OFF.items()):
            value = options.get(option)
            if value == "OFF":
                continue
            if value is None:
                fail(
                    f"{option} is not stated: its dependency is not in the CI image "
                    f"({probe}), OpenCV's default for it is ON, so the configure "
                    "fails the verification"
                )
            else:
                fail(f"{option}={value} but its dependency is not in the CI image ({probe})")

    # 2. Options that break the installed package without ever reaching the
    #    verification, because they carry no `VERIFY` clause.
    for option, consequence in sorted(REQUIRED_OFF_UNVERIFIED.items()):
        if options.get(option) != "OFF":
            fail(
                f"{option} has to be OFF: {consequence}"
            )

    # 3. An option OpenCV does not declare is silently ignored, so the intended
    #    "off" falls back to the default and the verification fails elsewhere.
    for option in sorted(options):
        if option.startswith("WITH_") and option not in DECLARED_WITH_OPTIONS:
            fail(
                f"{option} is not an option OpenCV 4.12 declares, so cmake ignores it; "
                "remove it or fix the spelling"
            )

    # 4. Hermeticity. Both of these pull an archive from the network in the
    #    middle of the configure step, which this build may not do.
    if options.get("WITH_IPP") != "OFF":
        fail("WITH_IPP has to be OFF: with it on, OpenCV downloads the IPPICV "
             "archive (cmake/OpenCVFindIPP.cmake) while configuring")
    if options.get("OBSENSOR_USE_ORBBEC_SDK") not in (None, "OFF"):
        fail("OBSENSOR_USE_ORBBEC_SDK has to stay OFF: with it on, OpenCV "
             "downloads the Orbbec SDK (3rdparty/orbbecsdk/orbbecsdk.cmake) "
             "while configuring")

    if _failures:
        print(f"{_failures} OpenCV option check(s) failed")
        return 1
    print(
        "OpenCV option check passed "
        f"({len(REQUIRED_OFF)} options that cannot be satisfied, "
        f"{len(REQUIRED_OFF_UNVERIFIED)} export traps, "
        f"{len(DECLARED_WITH_OPTIONS)} declared options, {len(packages)} apt packages)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
