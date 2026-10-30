#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTDOCUMENTFORMATTING_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTDOCUMENTFORMATTING_H

#include <QColor>
#include <QImage>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QVector>

namespace snow_shot::presentation {

class ScreenshotInertDocument final : public QTextDocument {
  public:
    explicit ScreenshotInertDocument(QObject* parent = nullptr) : QTextDocument(parent) {}

  protected:
    QVariant loadResource(int type, const QUrl&) override {
        // An invalid QVariant makes Qt's image handler fall back to opening the URL.
        return type == QTextDocument::ImageResource ? QVariant::fromValue(QImage()) : QVariant();
    }
};

// Resolved values only: document formatting can run on an import worker without
// consulting a widget, palette, or theme manager.
struct ScreenshotDocumentStyle final {
    QColor text;
    QColor link;
    QString monospaceFamily;
    QColor codeBackground;
    QColor secondaryText;
    QColor border;
};

inline void setScreenshotDocumentStyle(QTextDocument& document,
                                       const ScreenshotDocumentStyle& style,
                                       bool preserveHtmlStyles = false) {
    document.setDefaultStyleSheet(
        (preserveHtmlStyles
             ? QString()
             : QStringLiteral("body,pre,code { color: %1; }").arg(style.text.name())) +
        QStringLiteral(
            "p { margin-top: 6px; margin-bottom: 6px; } "
            "h1,h2,h3 { margin-top: 12px; margin-bottom: 8px; } "
            "a { color: %1; } pre,code { font-family: '%2'; background-color: %3; } "
            "blockquote { margin-left: 16px; color: %4; } "
            "table { border-collapse: collapse; } td,th { border: 1px solid %5; padding: 6px; }")
            .arg(style.link.name(), style.monospaceFamily, style.codeBackground.name(),
                 style.secondaryText.name(), style.border.name()));
}

inline void setScreenshotDocumentSource(QTextDocument& document, const QString& source,
                                        bool markdown, const ScreenshotDocumentStyle& style,
                                        bool preserveHtmlStyles = false) {
    if (markdown)
        document.setMarkdown(source, QTextDocument::MarkdownDialectGitHub);
    else
        document.setHtml(source);
    if (!markdown && preserveHtmlStyles)
        return;

    // Markdown import does not apply the HTML stylesheet to code blocks or links.
    QVector<QTextCursor> links;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
            const auto text = fragment.fragment();
            if (text.isValid() && text.charFormat().isAnchor()) {
                QTextCursor link(&document);
                link.setPosition(text.position());
                link.setPosition(text.position() + text.length(), QTextCursor::KeepAnchor);
                links.push_back(link);
            }
        }
        auto blockFormat = block.blockFormat();
        if (blockFormat.hasProperty(QTextFormat::BlockCodeFence) ||
            blockFormat.hasProperty(QTextFormat::BlockCodeLanguage) ||
            blockFormat.nonBreakableLines()) {
            QTextCursor code(block);
            blockFormat.setBackground(style.codeBackground);
            blockFormat.setLeftMargin(8);
            blockFormat.setRightMargin(8);
            code.setBlockFormat(blockFormat);
            code.select(QTextCursor::BlockUnderCursor);
            QTextCharFormat characters;
            characters.setFontFamilies({style.monospaceFamily});
            characters.setForeground(style.text);
            characters.setBackground(style.codeBackground);
            code.mergeCharFormat(characters);
        }
    }
    QTextCharFormat linkFormat;
    linkFormat.setForeground(style.link);
    for (auto& link : links)
        link.mergeCharFormat(linkFormat);
}

} // namespace snow_shot::presentation

#endif
