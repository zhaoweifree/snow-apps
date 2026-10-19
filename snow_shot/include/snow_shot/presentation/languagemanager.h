#ifndef SNOW_SHOT_PRESENTATION_LANGUAGEMANAGER_H
#define SNOW_SHOT_PRESENTATION_LANGUAGEMANAGER_H

#include <QLocale>
#include <QHash>
#include <QList>
#include <QObject>
#include <QTranslator>
#include <QString>

#include <memory>

namespace snow_shot::presentation {
struct LanguageCatalog {
    QString localeName;
    QString nativeName;
};

class LanguageManager final : public QObject {
    Q_OBJECT

  public:
    static LanguageManager& instance();

    void initialize();

    [[nodiscard]] QList<LanguageCatalog> availableLanguages() const;
    [[nodiscard]] QString languagePreference() const;
    [[nodiscard]] QLocale currentLocale() const;
    [[nodiscard]] bool setLanguage(const QString& languagePreference);

  signals:
    void languageChanged(const QString& preference, const QLocale& effectiveLocale);
    void languageChangeFailed(const QString& preference);

  private:
    explicit LanguageManager(QObject* parent = nullptr);

    [[nodiscard]] bool applyPreference(const QString& preference, bool persist);
    void discoverCatalogs();

    // Qt5 上补装一份 Qt 自带目录的翻译（标准对话框/标准按钮用）。
    // Qt6 走 MERGE_QT_TRANSLATIONS 把 qtbase_<locale>.qm 并进 snow_shot_<locale>.qm，
    // 不需要这一层；Qt5 垫片没有等价合并，所以这里单独装。
    void installQtStockTranslator(const QLocale& locale);

    QList<LanguageCatalog> m_availableLanguages;
    QHash<QString, QString> m_catalogResources;
    QHash<QString, QLocale> m_catalogLocales;
    QString m_languagePreference = QStringLiteral("system");
    QLocale m_currentLocale = QLocale(QLocale::English, QLocale::UnitedStates);
    std::unique_ptr<QTranslator> m_translator;
    std::unique_ptr<QTranslator> m_qtTranslator;
    bool m_qtStockTranslatorMissingLogged = false;
    bool m_initialized = false;
};
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_LANGUAGEMANAGER_H
