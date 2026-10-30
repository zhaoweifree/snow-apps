# Windows crosshair cursors

Snow Shot replaces its Windows crosshair cursors with a transparent bitmap containing a white
cross and a black outline. The shared 32×32 RGBA artwork has hotspot `(16,16)`. Qt scales this
artwork for the current display; capture scales it to the observed native cursor dimensions.
Both use nearest-neighbor sampling. Qt keeps the logical hotspot `(16,16)` and an even physical
raster centered on Qt's rounded hotspot, preventing a one-pixel offset at custom display scales.
Capture preserves the observed native dimensions and rounds its scaled hotspot up to the white
center pixel, including odd physical raster sizes.

Windows' stock `IDC_CROSS` may use monochrome AND/XOR pixels that invert the desktop beneath
the cursor. Snow Shot replaces that stock cursor when saving screenshots and recording frames.
Copies of the stock resource with identical pixels are also substituted. Other application-owned
cursors, including the fixture's colored marker and Snow Shot's brush previews, retain their own
rendering. Capture does not change another application's live pointer.

## Build and launch the stock cursor fixture

From the repository root in PowerShell:

```powershell
scripts/build.ps1 -Preset windows-msvc-debug -Target snow-shot-recapture-cursor-native-e2e-test
& .\build\windows-msvc-debug\snow_shot\test-bin\Debug\snow-shot-recapture-cursor-native-e2e-test.exe `
  --run-native --native-editor --stock-crosshair -platform windows
```

The external Win32 window is titled **Native cursor regression**. Its client area contains black,
gray, and white vertical bands and uses the actual shared `LoadCursorW(nullptr, IDC_CROSS)` cursor.
Move the pointer across the bands to observe the native cursor's background-dependent appearance.
Drag the title bar to move the fixture to another monitor. Close the window or press Escape while
it has keyboard focus to exit.

The fixture borrows the stock cursor and never calls `DestroyCursor` on it. Its default marker mode
creates and destroys its own cursor after destroying the window and unregistering its class.
Geometry inspection uses `GetIconInfo`; the returned color/mask bitmaps are released with
`DeleteObject`. The shared RGBA view remains valid for the process lifetime. Snow Shot caches Qt
cursors and separately owns `CreateIconIndirect` cursor handles for immediate native selection.
It deletes the temporary color/mask bitmaps after creation and calls `DestroyCursor` on these
owned handles when the application cache is destroyed.

## Reproduce screenshots and recordings

1. Launch Snow Shot with `scripts/run-snow-shot.ps1 -Preset windows-msvc-debug`, then launch the
   stock fixture using the command above.
2. In Snow Shot's screenshot settings, choose **DXGI** for **Screenshot capture API** and enable
   **Show Cursor**. Place the pointer over the gray band, start a normal screenshot with the
   configured shortcut, and save it. The saved pointer must have a white fill and black outline.
3. Start another screenshot and use its recapture action while the pointer is over the fixture.
   Check that the selection cursor and the captured fixture cursor both use the custom artwork.
   Hiding the captured cursor must remove only the cursor layer.
4. Choose **DXGI** for **Recording capture API**. Record an area containing the fixture with the
   recording cursor control enabled. Move the pointer across all three bands, stop, and inspect
   the recording. The cursor colors must remain white and black throughout. Repeat with the cursor
   control disabled and check that no pointer is recorded.
5. Repeat the screenshot and recording checks with **WGC** and **GDI**. Native cursor sampling
   covers these paths; Desktop Duplication also matches its pointer shape to the stock resource.
6. Repeat at Windows display scales of **100%**, **125%**, **150%**, and **200%**. Restart the fixture after
   changing a display's scale. On mixed-DPI systems, move the fixture to each display and also move
   Snow Shot's pinned image/recording canvas between displays. Check the cursor's size, centered
   hotspot, and stable colors. Repeated screenshot selection updates must not flicker.

The fixture uses temporary windows only. The automated test below isolates its Snow Shot settings,
restores the pointer position and confinement, and shuts down capture when it finishes.

## Focused validation

The native test requires an unlocked Windows desktop and moves/confines the pointer while it runs.
Run only this regression:

```powershell
ctest --test-dir build/windows-msvc-debug -C Debug --output-on-failure `
  -R '^snow-shot-recapture-stock-crosshair-native-e2e-test$'
```

The equivalent direct command, also useful before CTest registration has been regenerated, is:

```powershell
& .\build\windows-msvc-debug\snow_shot\test-bin\Debug\snow-shot-recapture-cursor-native-e2e-test.exe `
  --run-native --native-target --stock-crosshair -platform windows
```

The regression captures the naturally entered stock cursor, covers it with an application-owned
green marker, hands input back to the external window, and recaptures without moving the pointer.
It compares the cursor patch against the shared artwork composited over the captured desktop,
using the stock cursor's actual physical dimensions and the custom artwork's scaled hotspot.
It also checks that restoring input restores the green marker. This provides a negative control
for accidentally replacing unrelated cursors.

Evidence is saved under `cursor-e2e-evidence/stock-crosshair/` relative to the working directory:
`natural.png`, `overlay.png`, `recapture.png`, and `restored.png`. The log reports capture backend,
native cursor size/hotspot, normalized hotspot, and whether the custom pixels matched. CTest's
working directory is the target's build directory. A setup/capture failure is not a passing run.
