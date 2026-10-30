#include "snow_shot/presentation/screenshotselectionexportworkflow.h"

#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "../pinned/screenshotpintoperfinstrumentation.h"

#include <memory>

namespace {
ScreenshotResultStyle resultStyle(const ScreenshotSelectionModel& selection) {
    return selection.resultStyle();
}
} // namespace

ScreenshotSelectionExportWorkflow::ScreenshotSelectionExportWorkflow(
    ScreenshotSelectionExportWorkflowContext context)
    : m_context(context) {}

bool ScreenshotSelectionExportWorkflow::copySelectionToClipboard(ResultValidator validator,
                                                                 CopyCompletion completion,
                                                                 quint64 publicationId) {
    const QRect selection = m_context.selection.pixelSelection();
    if (m_context.selection.constructionActive() || m_context.selection.regionOperationActive() ||
        selection.width() < 1 || selection.height() < 1) {
        return false;
    }

    return m_context.imageComposer.requestSelectionClipboard(
        selection, resultStyle(m_context.selection), &m_context.callbackContext,
        [this, publicationId, validator = std::move(validator),
         completion = std::move(completion)](ScreenshotSelectionClipboardResult result) mutable {
            if (validator && !validator()) {
                if (completion) {
                    completion(false, {});
                }
                return;
            }
            if (!result.isValid()) {
                if (completion) {
                    completion(false, {});
                }
                return;
            }
            QImage resultImage = std::move(result.image);
            auto terminal = std::make_shared<bool>(false);
            auto publishCompletion = std::make_shared<std::function<void(bool)>>();
            *publishCompletion = [terminal, validator = std::move(validator),
                                  completion = std::move(completion),
                                  resultImage = std::move(resultImage)](bool success) mutable {
                if (*terminal) {
                    return;
                }
                *terminal = true;
                if (validator && !validator()) {
                    success = false;
                }
                if (completion) {
                    completion(success, success ? std::move(resultImage) : QImage{});
                }
            };
            const bool scheduled = m_context.destination.publishClipboard(
                &m_context.callbackContext, std::move(result.payload),
                [publishCompletion](bool success) { (*publishCompletion)(success); },
                publicationId);
            if (!scheduled) {
                (*publishCompletion)(false);
            }
        },
        m_context.selection.exportLayoutCompaction());
}

bool ScreenshotSelectionExportWorkflow::pinSelectionToScreen(ResultValidator validator,
                                                             Completion completion) {
    SNOW_SHOT_PIN_PERF_SCOPE("workflow.pin_selection");
    const QRect selection = m_context.selection.pixelSelection();
    if (m_context.selection.constructionActive() || m_context.selection.regionOperationActive() ||
        selection.width() < 1 || selection.height() < 1) {
        return false;
    }

    std::optional<ScreenshotPinnedSelectionRequest> request =
        m_context.imageComposer.preparePinnedSelection(
            selection, resultStyle(m_context.selection),
            m_context.selection.exportLayoutCompaction());
    if (!request.has_value()) {
        return false;
    }
    if (m_context.cachedRecognitionResults) {
        request->recognitionResults = m_context.cachedRecognitionResults();
    }

    return m_context.imageComposer.schedulePinnedSelection(
        std::move(*request), &m_context.callbackContext,
        [this, validator = std::move(validator),
         completion = std::move(completion)](ScreenshotPinnedSelectionRequest pinRequest,
                                             ScreenshotPinnedSelectionResultHandle result) mutable {
            if (validator && !validator()) {
                result.cancel();
                if (completion) {
                    completion(false, {});
                }
                return;
            }
            if (!pinRequest.isPrepared()) {
                result.cancel();
                if (completion)
                    completion(false, {});
                return;
            }
            const auto terminal = std::make_shared<bool>(false);
            const auto finish = std::make_shared<std::function<void(bool, QImage)>>();
            *finish = [terminal, validator = std::move(validator),
                       completion = std::move(completion)](bool success, QImage image) mutable {
                if (*terminal)
                    return;
                *terminal = true;
                if (validator && !validator())
                    success = false;
                SNOW_SHOT_PIN_PERF_MILESTONE("workflow.destination_complete");
                if (completion)
                    completion(success, success ? std::move(image) : QImage{});
            };
            if (!m_context.destination.presentPinnedSelection(
                    pinRequest, std::move(result), [finish](bool success, QImage image) mutable {
                        (*finish)(success, std::move(image));
                    })) {
                (*finish)(false, {});
            }
        });
}
