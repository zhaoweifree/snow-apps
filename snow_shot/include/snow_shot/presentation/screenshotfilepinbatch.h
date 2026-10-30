#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTFILEPINBATCH_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTFILEPINBATCH_H

#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/platform/selectedfiles.h"
#include <QHash>
#include <QSet>
#include <QObject>
#include <QQueue>
#include <QStringList>

#include <functional>
#include <memory>

namespace snow_shot::platform {
class SelectedFileBackend;
struct SelectedFileTarget;
} // namespace snow_shot::platform

class ScreenshotFilePinBatch final : public QObject {
  public:
    using Present = std::function<bool(ScreenshotClipboardContent)>;
    using Source =
        std::function<snow_shot::platform::SelectedFileResult(const ScreenshotExportCancellation&)>;
    using Failure = std::function<void(snow_shot::platform::SelectedFileError)>;
    // True means at least one file was presented or handled as an existing pin. Explicit
    // cancellation discards this callback; natural failures invoke it after the batch stops.
    using Completion = std::function<void(bool)>;

    struct DuplicateFilter {
        QSet<QString> identities;
        // Runs on the GUI thread; false means the match disappeared and must be decoded.
        std::function<bool(const snow_shot::storage::PinnedSourceIdentity&)> consume;
    };
    explicit ScreenshotFilePinBatch(QObject* parent = nullptr);
    ~ScreenshotFilePinBatch() override;
    void start(QStringList paths, Present present, DuplicateFilter filter = {},
               Completion completion = {});
    void startSelection(std::shared_ptr<snow_shot::platform::SelectedFileBackend> backend,
                        snow_shot::platform::SelectedFileTarget target, Present present,
                        Failure failure = {}, DuplicateFilter filter = {});
    void cancel();
    [[nodiscard]] bool active() const {
        return m_active;
    }

  private:
    void startSource(Source source, Present present, Failure failure = {},
                     DuplicateFilter filter = {}, Completion completion = {});
    void finish(bool succeeded);
    void submitDecode(quint64 generation, qsizetype index);
    void submitPendingDecodes(quint64 generation);
    void dispatch(quint64 generation);

    snow_shot::storage::PinnedTextStyle m_textStyle;
    PinnedColorSwatchStyle m_colorSwatchStyle;
    ScreenshotExportJobHandle m_snapshotJob;
    QHash<qsizetype, ScreenshotExportJobHandle> m_decodeJobs;
    quint64 m_generation = 0;
    QList<ScreenshotClipboardLocalImage> m_files;
    // Completed decodes keyed by file index; out-of-order completions wait in
    // their slot so presentation always follows source order.
    QHash<qsizetype, std::optional<ScreenshotClipboardContent>> m_ready;
    qsizetype m_nextSubmit = 0;
    qsizetype m_nextPresent = 0;
    Present m_present;
    Completion m_completion;
    bool m_succeeded = false;
    DuplicateFilter m_duplicateFilter;
    QSet<qsizetype> m_skippedDuplicates;
    bool m_active = false;
    bool m_dispatching = false;
    bool m_redispatch = false;
};

// Shell verbs may launch one process per selected file. Keep those requests independent of
// replaceable clipboard/drop batches, while limiting image decoding to one request at a time.
class ScreenshotFilePinQueue final : public QObject {
  public:
    struct Options {
        ScreenshotFilePinBatch::Present present;
        ScreenshotFilePinBatch::DuplicateFilter duplicateFilter;
    };
    using Prepare = std::function<Options()>;
    using Failure = std::function<void(const QString&)>;

    explicit ScreenshotFilePinQueue(QObject* parent = nullptr);
    void enqueue(QStringList paths, Prepare prepare, Failure failure);
    void cancel();
    [[nodiscard]] bool active() const {
        return m_running || !m_requests.isEmpty();
    }

  private:
    struct Request {
        QString path;
        Prepare prepare;
        Failure failure;
    };
    void startNext();

    ScreenshotFilePinBatch m_batch;
    QQueue<Request> m_requests;
    bool m_running = false;
};
#endif
