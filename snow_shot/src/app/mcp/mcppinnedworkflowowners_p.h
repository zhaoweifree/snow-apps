#ifndef SNOW_SHOT_APP_MCP_MCPPINNEDWORKFLOWOWNERS_P_H
#define SNOW_SHOT_APP_MCP_MCPPINNEDWORKFLOWOWNERS_P_H

#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <utility>

namespace snow_shot::app::mcp {

// A connection owns cancellation only while its pin has outstanding workflow work.
class McpPinnedWorkflowOwners final : public QObject {
  public:
    explicit McpPinnedWorkflowOwners(QObject* parent = nullptr) : QObject(parent) {}
    ~McpPinnedWorkflowOwners() override {
        cancelAll();
    }

    void track(quint64 connection, ScreenshotPinnedWindow* window) {
        if (!window)
            return;
        release(window);
        if (!window->automationWorkflowBusy())
            return;
        Entry entry;
        entry.connection = connection;
        entry.window = window;
        // Recognition can synchronously hand off to rendering or translation.
        // Inspect the settled state after that transition, retaining cancellation
        // ownership throughout the handoff.
        entry.changed = connect(
            window, &ScreenshotPinnedWindow::automationWorkflowStateChanged, this,
            [this, window] {
                const auto found = m_entries.constFind(window);
                if (found != m_entries.cend() &&
                    (!found->window || !found->window->automationWorkflowBusy()))
                    release(window);
            },
            Qt::QueuedConnection);
        entry.closing = connect(window, &ScreenshotPinnedWindow::closingForPersistence, this,
                                [this, window] { release(window); });
        entry.destroyed =
            connect(window, &QObject::destroyed, this, [this, window] { release(window); });
        m_entries.insert(window, std::move(entry));
    }

    void cancel(quint64 connection) {
        QList<QPointer<ScreenshotPinnedWindow>> windows;
        const auto entries = m_entries;
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            if (it->connection == connection) {
                windows.append(it->window);
                release(it.key());
            }
        }
        for (const auto& window : windows)
            if (window)
                window->cancelAutomationRecognition();
    }

    void cancelAll() {
        QList<QPointer<ScreenshotPinnedWindow>> windows;
        const auto entries = m_entries;
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            windows.append(it->window);
            release(it.key());
        }
        for (const auto& window : windows)
            if (window)
                window->cancelAutomationRecognition();
    }

    [[nodiscard]] qsizetype size() const {
        return m_entries.size();
    }

  private:
    struct Entry {
        quint64 connection = 0;
        QPointer<ScreenshotPinnedWindow> window;
        QMetaObject::Connection changed;
        QMetaObject::Connection closing;
        QMetaObject::Connection destroyed;
    };
    void release(ScreenshotPinnedWindow* window) {
        const auto entry = m_entries.take(window);
        disconnect(entry.changed);
        disconnect(entry.closing);
        disconnect(entry.destroyed);
    }
    QHash<ScreenshotPinnedWindow*, Entry> m_entries;
};

} // namespace snow_shot::app::mcp
#endif
