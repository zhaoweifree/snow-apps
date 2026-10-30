// SPDX-License-Identifier: GPL-3.0-only
#include "imageimportsourcereader_p.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/pinnedwindowrepository.h"

#include <QFileInfo>
#include <algorithm>
#include <utility>

namespace snow_shot::presentation {

QImage readImageImportFile(const QString& path,
                           const std::optional<ScreenshotClipboardLocalImage>& snapshot,
                           ScreenshotClipboardContentReader::CancellationCheck cancelled,
                           ScreenshotClipboardContentReader::AllocationCheck allocate) {
    auto file = snapshot;
    if (!file && ScreenshotClipboardContentReader::supportedImageFileExtensions().contains(
                     QFileInfo(path).suffix().toLower())) {
        const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles({path}, cancelled);
        if (!files.isEmpty())
            file = files.first();
    }
    return file ? ScreenshotClipboardContentReader::decodeImageFile(*file, std::move(cancelled),
                                                                    std::move(allocate))
                : QImage();
}

ImageImportSource
readImageImportPinnedRecord(const QString& recordId,
                            ScreenshotClipboardContentReader::CancellationCheck cancelled,
                            ScreenshotClipboardContentReader::AllocationCheck allocate,
                            std::optional<PinnedColorSwatchStyle> swatchStyle) {
    ImageImportSource result;
    if (cancelled && cancelled())
        return result;
    qint64 allocatedBytes = 0;
    const auto reserve = [&allocate, &allocatedBytes](qint64 bytes) {
        // Pin restoration and original-text rendering can request different transient capacities.
        // Keep the largest reservation until preparation replaces it with retained image bytes.
        allocatedBytes = std::max(allocatedBytes, bytes);
        return !allocate || allocate(allocatedBytes);
    };
    const auto record = storage::ApplicationStorage::instance().pinnedWindows().loadPreviewSource(
        recordId, reserve, cancelled);
    if (!record || (cancelled && cancelled()))
        return result;
    result.originalFileName = record->originalFileName;
    result.createdUtc = record->createdUtc;
    if (record->sourceKind == storage::PinnedWindowSourceKind::ClipboardText) {
        ScreenshotClipboardOriginalContent original;
        original.html = record->originalHtml;
        original.text = record->originalText;
        original.textStyle = record->textStyle;
        original.textFormat = record->textFormat;
        auto rendered = ScreenshotClipboardContentReader::renderOriginalText(
            original, record->firstCreationTextDpi, {}, reserve, std::move(swatchStyle));
        if (rendered)
            result.image = std::move(rendered->image);
    } else {
        result.image = record->image;
    }
    return result;
}

QImage readImageImportClipboard(ScreenshotClipboardContentSnapshot snapshot,
                                ScreenshotClipboardContentReader::CancellationCheck cancelled,
                                ScreenshotClipboardContentReader::AllocationCheck allocate) {
    auto content = ScreenshotClipboardContentReader::decode(
        std::move(snapshot), std::move(cancelled), std::move(allocate));
    // Retain only native pixels. Formatted documents and other clipboard payloads are unnecessary
    // once the editor has accepted the decoded source.
    return content ? std::move(content->image) : QImage();
}

} // namespace snow_shot::presentation
