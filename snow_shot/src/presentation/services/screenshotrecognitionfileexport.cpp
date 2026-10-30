#include "snow_shot/presentation/screenshotrecognitionfileexport.h"
#include "snow_shot/storage/exportfilename.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QTemporaryFile>
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
#include <QScopeGuard>
#include <QXmlStreamWriter>
#include <mz.h>
#include <mz_strm.h>
#include <mz_strm_mem.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>
#include <algorithm>
#endif

namespace {
QString message(const char* source) {
    return QCoreApplication::translate("ScreenshotRecognitionFileExport", source);
}

#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
QString cellReference(int row, int column) {
    QString reference;
    for (++column; column > 0; column = (column - 1) / 26)
        reference.prepend(QChar(u'A' + (column - 1) % 26));
    return reference + QString::number(row + 1);
}

QString spreadsheetText(const QString& text) {
    QString result;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        // SpreadsheetML escapes control characters and literal escape sequences separately.
        const bool escapePrefix =
            character == u'_' && index + 6 < text.size() && text.at(index + 1) == u'x' &&
            text.at(index + 6) == u'_' &&
            std::all_of(text.cbegin() + index + 2, text.cbegin() + index + 6, [](QChar value) {
                return (value >= u'0' && value <= u'9') || (value >= u'a' && value <= u'f') ||
                       (value >= u'A' && value <= u'F');
            });
        if (escapePrefix ||
            (character.unicode() < 0x20 && character != u'\n' && character != u'\t') ||
            character == QChar(0xfffe) || character == QChar(0xffff)) {
            result += QStringLiteral("_x%1_").arg(
                QString::number(static_cast<uint>(character.unicode()), 16)
                    .rightJustified(4, QLatin1Char('0'))
                    .toUpper());
        } else {
            result += character;
        }
    }
    return result;
}

QByteArray tableWorkbook(const ScreenshotTableDocument& document) {
    if (document.empty() || document.rowCount() > 1048576 || document.columnCount() > 16384)
        return {};
    void* stream = mz_stream_mem_create();
    if (!stream)
        return {};
    const auto streamGuard = qScopeGuard([&] { mz_stream_mem_delete(&stream); });
    if (mz_stream_mem_open(stream, nullptr, MZ_OPEN_MODE_CREATE | MZ_OPEN_MODE_READWRITE) != MZ_OK)
        return {};
    void* writer = mz_zip_writer_create();
    if (!writer)
        return {};
    const auto writerGuard = qScopeGuard([&] { mz_zip_writer_delete(&writer); });
    if (mz_zip_writer_open(writer, stream, 0) != MZ_OK)
        return {};
    const auto addXml = [writer](const char* name, const QString& root,
                                 const QString& nameSpace,
                                 const auto& fill) {
        QByteArray bytes;
        QXmlStreamWriter xml(&bytes);
        xml.writeStartDocument();
        xml.writeStartElement(root);
        xml.writeDefaultNamespace(nameSpace);
        fill(xml);
        xml.writeEndElement();
        xml.writeEndDocument();
        if (xml.hasError())
            return false;
        mz_zip_file entry{};
        entry.filename = name;
        entry.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
        return mz_zip_writer_add_buffer(writer, bytes.constData(),
                                        static_cast<int32_t>(bytes.size()), &entry) == MZ_OK;
    };
    const auto spreadsheetNamespace =
        QStringLiteral("http://schemas.openxmlformats.org/spreadsheetml/2006/main");
    const auto relationshipNamespace =
        QStringLiteral("http://schemas.openxmlformats.org/package/2006/relationships");
    const auto relationship = [](QXmlStreamWriter& xml, const QString& id, const QString& type,
                                 const QString& target) {
        xml.writeEmptyElement(QStringLiteral("Relationship"));
        xml.writeAttribute(QStringLiteral("Id"), id);
        xml.writeAttribute(QStringLiteral("Type"), QStringLiteral("http://schemas.openxmlformats.org/"
                                                   "officeDocument/2006/relationships/") +
                                        type);
        xml.writeAttribute(QStringLiteral("Target"), target);
    };
    if (!addXml(
            "[Content_Types].xml", QStringLiteral("Types"),
            QStringLiteral("http://schemas.openxmlformats.org/package/2006/content-types"),
            [](QXmlStreamWriter& xml) {
                xml.writeEmptyElement(QStringLiteral("Default"));
                xml.writeAttribute(QStringLiteral("Extension"), QStringLiteral("rels"));
                xml.writeAttribute(QStringLiteral("ContentType"),
                                   QStringLiteral("application/vnd.openxmlformats-package.relationships+xml"));
                xml.writeEmptyElement(QStringLiteral("Default"));
                xml.writeAttribute(QStringLiteral("Extension"), QStringLiteral("xml"));
                xml.writeAttribute(QStringLiteral("ContentType"), QStringLiteral("application/xml"));
                const auto part = [&](const QString& name, const QString& type) {
                    xml.writeEmptyElement(QStringLiteral("Override"));
                    xml.writeAttribute(QStringLiteral("PartName"), name);
                    xml.writeAttribute(QStringLiteral("ContentType"), type);
                };
                part(QStringLiteral("/xl/workbook.xml"),
                     QStringLiteral("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"));
                part(QStringLiteral("/xl/worksheets/sheet1.xml"),
                     QStringLiteral("application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"));
                part(QStringLiteral("/xl/styles.xml"),
                     QStringLiteral("application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"));
            }) ||
        !addXml("_rels/.rels", QStringLiteral("Relationships"), relationshipNamespace,
                [&](QXmlStreamWriter& xml) {
                    relationship(xml, QStringLiteral("rId1"), QStringLiteral("officeDocument"), QStringLiteral("xl/workbook.xml"));
                }) ||
        !addXml("xl/workbook.xml", QStringLiteral("workbook"), spreadsheetNamespace,
                [](QXmlStreamWriter& xml) {
                    xml.writeNamespace(
                        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships"),
                        QStringLiteral("r"));
                    xml.writeStartElement(QStringLiteral("sheets"));
                    xml.writeEmptyElement(QStringLiteral("sheet"));
                    xml.writeAttribute(QStringLiteral("name"), message(QT_TRANSLATE_NOOP(
                                                    "ScreenshotRecognitionFileExport", "Table")));
                    xml.writeAttribute(QStringLiteral("sheetId"), QStringLiteral("1"));
                    xml.writeAttribute(QStringLiteral("r:id"), QStringLiteral("rId1"));
                    xml.writeEndElement();
                }) ||
        !addXml("xl/_rels/workbook.xml.rels", QStringLiteral("Relationships"), relationshipNamespace,
                [&](QXmlStreamWriter& xml) {
                    relationship(xml, QStringLiteral("rId1"), QStringLiteral("worksheet"), QStringLiteral("worksheets/sheet1.xml"));
                    relationship(xml, QStringLiteral("rId2"), QStringLiteral("styles"), QStringLiteral("styles.xml"));
                }) ||
        !addXml("xl/styles.xml", QStringLiteral("styleSheet"), spreadsheetNamespace, [](QXmlStreamWriter& xml) {
            xml.writeStartElement(QStringLiteral("fonts"));
            xml.writeAttribute(QStringLiteral("count"), QStringLiteral("2"));
            xml.writeEmptyElement(QStringLiteral("font"));
            xml.writeStartElement(QStringLiteral("font"));
            xml.writeEmptyElement(QStringLiteral("b"));
            xml.writeEndElement();
            xml.writeEndElement();
            xml.writeStartElement(QStringLiteral("fills"));
            xml.writeAttribute(QStringLiteral("count"), QStringLiteral("2"));
            for (const auto pattern : {QStringLiteral("none"), QStringLiteral("gray125")}) {
                xml.writeStartElement(QStringLiteral("fill"));
                xml.writeEmptyElement(QStringLiteral("patternFill"));
                xml.writeAttribute(QStringLiteral("patternType"), pattern);
                xml.writeEndElement();
            }
            xml.writeEndElement();
            xml.writeStartElement(QStringLiteral("borders"));
            xml.writeAttribute(QStringLiteral("count"), QStringLiteral("1"));
            xml.writeEmptyElement(QStringLiteral("border"));
            xml.writeEndElement();
            for (const auto group : {QStringLiteral("cellStyleXfs"), QStringLiteral("cellXfs")}) {
                const bool cells = group == QStringLiteral("cellXfs");
                xml.writeStartElement(group);
                xml.writeAttribute(QStringLiteral("count"), cells ? QStringLiteral("2") : QStringLiteral("1"));
                for (int index = 0; index < (cells ? 2 : 1); ++index) {
                    xml.writeStartElement(QStringLiteral("xf"));
                    xml.writeAttribute(QStringLiteral("numFmtId"), QStringLiteral("49"));
                    xml.writeAttribute(QStringLiteral("fontId"), QString::number(index));
                    xml.writeAttribute(QStringLiteral("fillId"), QStringLiteral("0"));
                    xml.writeAttribute(QStringLiteral("borderId"), QStringLiteral("0"));
                    if (cells) {
                        xml.writeAttribute(QStringLiteral("xfId"), QStringLiteral("0"));
                        xml.writeAttribute(QStringLiteral("applyNumberFormat"), QStringLiteral("1"));
                        xml.writeAttribute(QStringLiteral("applyFont"), QStringLiteral("1"));
                        xml.writeAttribute(QStringLiteral("applyAlignment"), QStringLiteral("1"));
                        xml.writeEmptyElement(QStringLiteral("alignment"));
                        xml.writeAttribute(QStringLiteral("wrapText"), QStringLiteral("1"));
                        xml.writeAttribute(QStringLiteral("vertical"), QStringLiteral("top"));
                    }
                    xml.writeEndElement();
                }
                xml.writeEndElement();
            }
            xml.writeStartElement(QStringLiteral("cellStyles"));
            xml.writeAttribute(QStringLiteral("count"), QStringLiteral("1"));
            xml.writeEmptyElement(QStringLiteral("cellStyle"));
            xml.writeAttribute(QStringLiteral("name"), QStringLiteral("Normal"));
            xml.writeAttribute(QStringLiteral("xfId"), QStringLiteral("0"));
            xml.writeAttribute(QStringLiteral("builtinId"), QStringLiteral("0"));
            xml.writeEndElement();
        }))
        return {};
    bool validCells = true;
    if (!addXml("xl/worksheets/sheet1.xml", QStringLiteral("worksheet"), spreadsheetNamespace,
                [&](QXmlStreamWriter& xml) {
                    xml.writeEmptyElement(QStringLiteral("dimension"));
                    xml.writeAttribute(QStringLiteral("ref"), QStringLiteral("A1:") +
                                                   cellReference(document.rowCount() - 1,
                                                                 document.columnCount() - 1));
                    xml.writeStartElement(QStringLiteral("sheetData"));
                    QStringList merges;
                    for (int row = 0; row < document.rowCount(); ++row) {
                        xml.writeStartElement(QStringLiteral("row"));
                        xml.writeAttribute(QStringLiteral("r"), QString::number(row + 1));
                        for (int column = 0; column < document.columnCount(); ++column) {
                            const auto* cell = document.anchorCellAt(row, column);
                            if (!cell)
                                continue;
                            if (cell->text.size() > 32767) {
                                validCells = false;
                                return;
                            }
                            const QString reference = cellReference(row, column);
                            xml.writeStartElement(QStringLiteral("c"));
                            xml.writeAttribute(QStringLiteral("r"), reference);
                            xml.writeAttribute(QStringLiteral("t"), QStringLiteral("inlineStr"));
                            xml.writeAttribute(QStringLiteral("s"), cell->header ? QStringLiteral("1") : QStringLiteral("0"));
                            xml.writeStartElement(QStringLiteral("is"));
                            xml.writeStartElement(QStringLiteral("t"));
                            xml.writeAttribute(QStringLiteral("xml:space"), QStringLiteral("preserve"));
                            xml.writeCharacters(spreadsheetText(cell->text));
                            xml.writeEndElement();
                            xml.writeEndElement();
                            xml.writeEndElement();
                            if (cell->rowSpan > 1 || cell->columnSpan > 1)
                                merges.append(reference + u':' +
                                              cellReference(row + cell->rowSpan - 1,
                                                            column + cell->columnSpan - 1));
                        }
                        xml.writeEndElement();
                    }
                    xml.writeEndElement();
                    if (!merges.isEmpty()) {
                        xml.writeStartElement(QStringLiteral("mergeCells"));
                        xml.writeAttribute(QStringLiteral("count"), QString::number(merges.size()));
                        for (const auto& range : merges) {
                            xml.writeEmptyElement(QStringLiteral("mergeCell"));
                            xml.writeAttribute(QStringLiteral("ref"), range);
                        }
                        xml.writeEndElement();
                    }
                }) ||
        !validCells || mz_zip_writer_close(writer) != MZ_OK)
        return {};
    const void* buffer = nullptr;
    int32_t length = 0;
    if (mz_stream_mem_get_buffer(stream, &buffer) != MZ_OK)
        return {};
    mz_stream_mem_get_buffer_length(stream, &length);
    return QByteArray(static_cast<const char*>(buffer), length);
}
#endif

QString reserveTemporaryPath(const QString& directory) {
    QTemporaryFile temporary(QDir(directory).filePath(QStringLiteral(".snow-shot-XXXXXX")));
    temporary.setAutoRemove(false);
    if (!temporary.open())
        return {};
    const QString path = temporary.fileName();
    temporary.close();
    return path;
}

QStringList existingPaths(const QStringList& paths) {
    QStringList existing;
    for (const QString& path : paths) {
        if (QFileInfo::exists(path))
            existing.push_back(path);
    }
    return existing;
}
} // namespace

QString ScreenshotRecognitionFileExport::extension(ScreenshotRecognitionFileKind kind) {
    switch (kind) {
    case ScreenshotRecognitionFileKind::Html:
        return QStringLiteral("html");
    case ScreenshotRecognitionFileKind::Markdown:
        return QStringLiteral("md");
    case ScreenshotRecognitionFileKind::Table:
        return QStringLiteral("xlsx");
    case ScreenshotRecognitionFileKind::Latex:
    case ScreenshotRecognitionFileKind::Qr:
        return QStringLiteral("txt");
    }
    return {};
}

QString ScreenshotRecognitionFileExport::dialogFilter(ScreenshotRecognitionFileKind kind) {
    switch (kind) {
    case ScreenshotRecognitionFileKind::Html:
        return message(
            QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "HTML document (*.html)"));
    case ScreenshotRecognitionFileKind::Markdown:
        return message(
            QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "Markdown document (*.md)"));
    case ScreenshotRecognitionFileKind::Table:
        return message(
            QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "Excel workbook (*.xlsx)"));
    case ScreenshotRecognitionFileKind::Latex:
    case ScreenshotRecognitionFileKind::Qr:
        return message(
            QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "Text document (*.txt)"));
    }
    return {};
}

QString ScreenshotRecognitionFileExport::normalizedPath(const QString& path,
                                                        ScreenshotRecognitionFileKind kind) {
    QString normalized = QDir::cleanPath(path.trimmed());
    if (path.trimmed().isEmpty())
        return {};
    const QString fileName = QFileInfo(normalized).fileName();
    const qsizetype dot = fileName.lastIndexOf(QLatin1Char('.'));
    if (dot > 0 && dot + 1 < fileName.size())
        normalized.chop(fileName.size() - dot);
    else if (normalized.endsWith(QLatin1Char('.')))
        normalized.chop(1);
    return normalized + QLatin1Char('.') + extension(kind);
}

QStringList ScreenshotRecognitionFileExport::outputPaths(const QString& primaryPath,
                                                         ScreenshotRecognitionFileKind kind) {
    if (primaryPath.isEmpty())
        return {};
    QStringList paths{primaryPath};
    if (kind == ScreenshotRecognitionFileKind::Html ||
        kind == ScreenshotRecognitionFileKind::Markdown) {
        QString companion = primaryPath;
        companion.chop(extension(kind).size());
        companion += QStringLiteral("txt");
        paths.push_back(companion);
    }
    return paths;
}

bool ScreenshotRecognitionFileExport::confirmOverwrite(QWidget* owner, const QStringList& paths) {
    const QStringList existing = existingPaths(paths);
    if (existing.isEmpty())
        return true;
    QMessageBox dialog(
        QMessageBox::Question,
        message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "Replace existing files?")),
        message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                  "Replace the existing file(s)?\n%1"))
            .arg(existing.join(QLatin1Char('\n'))),
        QMessageBox::Yes | QMessageBox::No, owner);
    snowMessageBoxDontUseNativeDialog(dialog);
    dialog.button(QMessageBox::Yes)
        ->setText(message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "Replace")));
    dialog.button(QMessageBox::No)
        ->setText(message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "Cancel")));
    dialog.setDefaultButton(QMessageBox::No);
    return dialog.exec() == QMessageBox::Yes;
}

ScreenshotRecognitionFileSaveResult
ScreenshotRecognitionFileExport::saveToPath(const ScreenshotRecognitionFileSnapshot& snapshot,
                                            const QString& path, bool allowOverwrite) {
    if (!snapshot.hasContent())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "No recognition result is available to save"))};
    const QString primary = normalizedPath(path, snapshot.kind);
    if (primary.isEmpty())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "No output file was selected"))};
    const QStringList paths = outputPaths(primary, snapshot.kind);
    const QString directory = QFileInfo(primary).absolutePath();
    if (!QDir().mkpath(directory))
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "The output directory could not be created"))};
    for (const QString& output : paths) {
        const QFileInfo information(output);
        if (information.exists() && !information.isFile())
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "An existing output file could not be replaced"))};
    }
    if (!allowOverwrite && !existingPaths(paths).isEmpty())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "An output file already exists"))};

    QByteArray bytes;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (snapshot.kind == ScreenshotRecognitionFileKind::Table) {
        bytes = tableWorkbook(*snapshot.table);
        if (bytes.isEmpty())
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "The Excel workbook could not be created"))};
    } else
#endif
        bytes = snapshot.source.toUtf8();
    QStringList staged;
    // QVector<QString>, not QStringList: Qt5's QStringList (a QList<QString>)
    // has neither a size constructor nor resize() (QtCore/qlist.h only offers
    // reserve()), while Qt6 added QStringList(qsizetype). QVector is the one
    // spelling that works on both - Qt6 aliases QVector to QList.
    QVector<QString> backups(paths.size());
    QStringList published;
    auto rollback = [&]() {
        for (const QString& publishedPath : published)
            QFile::remove(publishedPath);
        for (qsizetype index = 0; index < backups.size(); ++index) {
            if (!backups.at(index).isEmpty())
                QFile::rename(backups.at(index), paths.at(index));
        }
        for (const QString& stagedPath : staged)
            QFile::remove(stagedPath);
    };
    for (qsizetype index = 0; index < paths.size(); ++index) {
        const QString temporary = reserveTemporaryPath(directory);
        if (temporary.isEmpty()) {
            rollback();
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "The result file could not be staged"))};
        }
        staged.push_back(temporary);
        QFile file(temporary);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(bytes) != bytes.size() || !file.flush()) {
            const QString error = file.errorString();
            file.close();
            rollback();
            return {{}, error};
        }
        file.close();
    }
    if (!allowOverwrite && !existingPaths(paths).isEmpty()) {
        rollback();
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "An output file already exists"))};
    }
    for (qsizetype index = 0; index < paths.size(); ++index) {
        if (!QFileInfo::exists(paths.at(index)))
            continue;
        if (!allowOverwrite) {
            rollback();
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "An output file already exists"))};
        }
        if (!QFileInfo(paths.at(index)).isFile()) {
            rollback();
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "An existing output file could not be replaced"))};
        }
        const QString backup = reserveTemporaryPath(directory);
        if (backup.isEmpty() || !QFile::remove(backup) || !QFile::rename(paths.at(index), backup)) {
            if (!backup.isEmpty())
                QFile::remove(backup);
            rollback();
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "An existing output file could not be replaced"))};
        }
        backups[index] = backup;
    }
    for (qsizetype index = 0; index < paths.size(); ++index) {
        if (!QFile::rename(staged.at(index), paths.at(index))) {
            rollback();
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "The result file could not be published"))};
        }
        published.push_back(paths.at(index));
    }
    for (const QString& backup : backups) {
        if (!backup.isEmpty())
            QFile::remove(backup);
    }
    return {primary, {}};
}

ScreenshotRecognitionFileSaveResult
ScreenshotRecognitionFileExport::quickSave(const ScreenshotRecognitionFileSnapshot& snapshot,
                                           const QString& directory, const QString& baseName) {
    if (!snapshot.hasContent())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "No recognition result is available to save"))};
    if (directory.trimmed().isEmpty())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "The save directory is not configured"))};
    if (baseName.isEmpty() || baseName.contains(QLatin1Char('/')) ||
        baseName.contains(QLatin1Char('\\')))
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "The screenshot filename format is invalid"))};
    const QString target = QDir::cleanPath(directory.trimmed());
    if (!QDir().mkpath(target))
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "The output directory could not be created"))};
    for (int suffix = 0; suffix < 1000000; ++suffix) {
        const QString ending = (suffix == 0 ? QString() : QStringLiteral("_%1").arg(suffix)) +
                               QLatin1Char('.') + extension(snapshot.kind);
        // A Markdown companion has a longer extension than its primary file. Reserve enough
        // space for every output so the pair always shares the same bounded stem.
        QString longestEnding = ending;
        for (const QString& output : outputPaths(ending, snapshot.kind)) {
            if (output.toUtf8().size() > longestEnding.toUtf8().size())
                longestEnding = output;
        }
        QString stem = snow_shot::storage::composeExportFileName(baseName, longestEnding);
        stem.chop(longestEnding.size());
        const QString path = QDir(target).filePath(stem + ending);
        if (existingPaths(outputPaths(path, snapshot.kind)).isEmpty()) {
            const auto result = saveToPath(snapshot, path);
            if (result.error != message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                                          "An output file already exists")))
                return result;
        }
    }
    return {{},
            message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                      "No available output filename was found"))};
}
