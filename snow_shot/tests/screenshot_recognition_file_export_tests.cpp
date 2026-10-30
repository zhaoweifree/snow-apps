#include "snow_shot/presentation/screenshotrecognitionfileexport.h"
#include "recognition_workbook_test_support.h"

#include <QApplication>
#include <QAbstractButton>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>

#include <stdexcept>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "saved text file is unreadable");
    return file.readAll();
}

void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "text fixture cannot be written");
    require(file.write(bytes) == bytes.size(), "text fixture write failed");
}

void savesConversionPairsWithExactSource() {
    QTemporaryDir directory;
    require(directory.isValid(), "conversion save directory unavailable");
    for (const auto kind :
         {ScreenshotRecognitionFileKind::Html, ScreenshotRecognitionFileKind::Markdown}) {
        const QString source = kind == ScreenshotRecognitionFileKind::Html
                                   ? QStringLiteral("<h1>雪</h1>\n<p>Partial")
                                   : QStringLiteral("# 雪\n\n**Partial");
        const QString input = directory.filePath(QStringLiteral("result.other"));
        const auto saved = ScreenshotRecognitionFileExport::saveToPath({kind, source}, input, true);
        const QString primary = directory.filePath(
            QStringLiteral("result.") + ScreenshotRecognitionFileExport::extension(kind));
        require(saved.succeeded() && saved.path == primary,
                "conversion primary extension must match its source format");
        require(read(primary) == source.toUtf8() &&
                    read(directory.filePath(QStringLiteral("result.txt"))) == source.toUtf8(),
                "conversion primary and companion must preserve exact UTF-8 source");
    }
}

ScreenshotRecognitionFileSnapshot tableSnapshot() {
    ScreenshotRecognitionFileSnapshot snapshot;
    snapshot.kind = ScreenshotRecognitionFileKind::Table;
    snapshot.table = ScreenshotTableDocument::fromHtml(QStringLiteral(
        "<table><tr><th colspan=\"2\">Header</th><th rowspan=\"2\">Group</th></tr>"
        "<tr><td></td><td>42</td></tr><tr><td></td><td></td><td></td></tr></table>"));
    return snapshot;
}

void savesRealWorkbookWithLiteralCellsAndSpans() {
    QTemporaryDir directory;
    auto snapshot = tableSnapshot();
    const QString longNumber = QStringLiteral("00123456789012345678901234567890");
    snapshot.table->setCellText(1, 0, longNumber);
    snapshot.table->setCellText(1, 1, QStringLiteral("=SUM(A1:A2)"));
    snapshot.table->setCellText(2, 0, QStringLiteral("  雪 < & > \" ' 😀\nnext\tline  "));
    snapshot.table->setCellText(2, 1, QStringLiteral("_x0041_\r") + QChar(1));
    const auto saved = ScreenshotRecognitionFileExport::saveToPath(
        snapshot, directory.filePath(QStringLiteral("表格.png")));
    require(saved.succeeded() && saved.path.endsWith(QStringLiteral("表格.xlsx")),
            "table saves must normalize to an Excel extension");
    require(QDir(directory.path()).entryList(QDir::Files).size() == 1 &&
                ScreenshotRecognitionFileExport::outputPaths(saved.path, snapshot.kind) ==
                    QStringList{saved.path},
            "table export must create one workbook without a text companion");
    const auto package = recognition_workbook_test::parts(saved.path);
    for (const auto* name :
         {"[Content_Types].xml", "_rels/.rels", "xl/workbook.xml", "xl/_rels/workbook.xml.rels",
          "xl/styles.xml", "xl/worksheets/sheet1.xml"})
        require(package.contains(QString::fromLatin1(name)), "workbook part is missing");
    require(package.value(QStringLiteral("_rels/.rels")).contains("Target=\"xl/workbook.xml\"") &&
                package.value(QStringLiteral("xl/_rels/workbook.xml.rels"))
                    .contains("Target=\"worksheets/sheet1.xml\"") &&
                package.value(QStringLiteral("[Content_Types].xml"))
                    .contains("spreadsheetml.sheet.main+xml"),
            "package relationships and content types must describe an Excel workbook");
    const auto sheet = recognition_workbook_test::sheet(package);
    require(sheet.dimension == QStringLiteral("A1:C3") && sheet.cells.size() == 7 &&
                sheet.merges == QStringList{QStringLiteral("A1:B1"), QStringLiteral("C1:C2")} &&
                !sheet.cells.contains(QStringLiteral("B1")) &&
                !sheet.cells.contains(QStringLiteral("C2")),
            "dimensions and merge ranges must preserve table geometry without duplicate cells");
    require(sheet.cells.value(QStringLiteral("A2")) == longNumber &&
                sheet.cells.value(QStringLiteral("B2")) == QStringLiteral("=SUM(A1:A2)") &&
                sheet.cells.value(QStringLiteral("A3")) == snapshot.table->cellText(2, 0) &&
                sheet.cells.value(QStringLiteral("B3")) ==
                    QStringLiteral("_x005F_x0041__x000D__x0001_") &&
                sheet.cells.contains(QStringLiteral("C3")) &&
                sheet.cells.value(QStringLiteral("C3")).isEmpty(),
            "literal cells must preserve exact values, whitespace, and SpreadsheetML escapes");
    require(sheet.styles.value(QStringLiteral("A1")) == QStringLiteral("1") &&
                sheet.styles.value(QStringLiteral("A2")) == QStringLiteral("0") &&
                package.value(QStringLiteral("xl/styles.xml")).contains("<b/>") &&
                package.value(QStringLiteral("xl/styles.xml")).contains("wrapText=\"1\""),
            "header cells must be bold and multiline text must wrap");
}

void workbookSaveHandlesCollisionsOverwriteAndFailures() {
    QTemporaryDir directory;
    auto snapshot = tableSnapshot();
    const QString path = directory.filePath(QStringLiteral("Table.xlsx"));
    write(path, QByteArrayLiteral("original"));
    require(!ScreenshotRecognitionFileExport::saveToPath(snapshot, path).succeeded() &&
                read(path) == QByteArrayLiteral("original"),
            "workbook overwrite requires authorization");
    const auto quick = ScreenshotRecognitionFileExport::quickSave(snapshot, directory.path(),
                                                                  QStringLiteral("Table"));
    require(quick.succeeded() && quick.path.endsWith(QStringLiteral("Table_1.xlsx")),
            "quick save must avoid an existing workbook");
    const auto overwritten = ScreenshotRecognitionFileExport::saveToPath(snapshot, path, true);
    require(overwritten.succeeded() && !recognition_workbook_test::parts(path).isEmpty(),
            "authorized overwrite must publish a valid workbook");
    const auto original = read(path);
    snapshot.table->setCellText(1, 0, QString(32768, QLatin1Char('x')));
    require(!ScreenshotRecognitionFileExport::saveToPath(snapshot, path, true).succeeded() &&
                read(path) == original,
            "unsupported cell values must not truncate data or replace an existing workbook");
    snapshot.table.reset();
    require(!snapshot.hasContent() &&
                !ScreenshotRecognitionFileExport::saveToPath(snapshot, path, true).succeeded(),
            "an unavailable table must not produce any file");
    snapshot.table = ScreenshotTableDocument(2, 28);
    snapshot.table->setCellText(1, 27, QStringLiteral("AA boundary"));
    require(ScreenshotRecognitionFileExport::saveToPath(snapshot, path, true).succeeded() &&
                recognition_workbook_test::sheet(recognition_workbook_test::parts(path))
                        .cells.value(QStringLiteral("AB2")) == QStringLiteral("AA boundary"),
            "blank tables and references beyond column Z must remain valid");
    const QString blocked = directory.filePath(QStringLiteral("blocked.xlsx"));
    require(QDir().mkpath(blocked), "create blocked workbook output");
    require(!ScreenshotRecognitionFileExport::saveToPath(snapshot, blocked, true).succeeded() &&
                QFileInfo(blocked).isDir() &&
                QDir(directory.path())
                    .entryList({QStringLiteral(".snow-shot-*")}, QDir::Files | QDir::Hidden)
                    .isEmpty(),
            "failed workbook saves must preserve directories and leave no staging files");
    QTimer::singleShot(0, qApp, [] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* box = qobject_cast<QMessageBox*>(widget))
                box->button(QMessageBox::No)->click();
    });
    require(!ScreenshotRecognitionFileExport::confirmOverwrite(nullptr, {path}),
            "workbook overwrite can be canceled");
}

void savesQrValuesAsOneTextFile() {
    QTemporaryDir directory;
    require(directory.isValid(), "QR save directory unavailable");
    const QString source = QStringList{QStringLiteral("first"), QStringLiteral("雪"),
                                       QStringLiteral("https://example.test")}
                               .join(QLatin1Char('\n'));
    const auto saved = ScreenshotRecognitionFileExport::saveToPath(
        {ScreenshotRecognitionFileKind::Qr, source},
        directory.filePath(QStringLiteral("codes.html")));
    require(saved.succeeded() && QFileInfo(saved.path).suffix() == QStringLiteral("txt") &&
                read(saved.path) == source.toUtf8(),
            "QR values must retain their order in one UTF-8 text file");
    require(!QFileInfo::exists(directory.filePath(QStringLiteral("codes.html"))),
            "QR save must not produce an image or conversion file");
}

void savesLatexAsOneTextFile() {
    QTemporaryDir directory;
    require(directory.isValid(), "LaTeX save directory unavailable");
    const QString source = QStringLiteral("\\frac{a_b}{c^2} <x> & y\n+1");
    const auto saved = ScreenshotRecognitionFileExport::saveToPath(
        {ScreenshotRecognitionFileKind::Latex, source},
        directory.filePath(QStringLiteral("codes.html")));
    require(saved.succeeded() && QFileInfo(saved.path).suffix() == QStringLiteral("txt") &&
                read(saved.path) == source.toUtf8(),
            "LaTeX source must remain verbatim in one UTF-8 text file");
    require(!QFileInfo::exists(directory.filePath(QStringLiteral("codes.html"))),
            "LaTeX save must not produce an image or conversion file");
}

void quickSaveAvoidsEitherPairCollision() {
    QTemporaryDir directory;
    require(directory.isValid(), "quick save directory unavailable");
    write(directory.filePath(QStringLiteral("Auto.txt")), QByteArrayLiteral("keep"));
    const auto first = ScreenshotRecognitionFileExport::quickSave(
        {ScreenshotRecognitionFileKind::Markdown, QStringLiteral("# partial")}, directory.path(),
        QStringLiteral("Auto"));
    require(first.succeeded() && first.path.endsWith(QStringLiteral("Auto_1.md")) &&
                read(directory.filePath(QStringLiteral("Auto.txt"))) == QByteArrayLiteral("keep") &&
                read(directory.filePath(QStringLiteral("Auto_1.txt"))) ==
                    QByteArrayLiteral("# partial"),
            "a companion collision must move the entire quick-save pair to a new base name");
    const auto second = ScreenshotRecognitionFileExport::quickSave(
        {ScreenshotRecognitionFileKind::Markdown, QStringLiteral("# next")}, directory.path(),
        QStringLiteral("Auto"));
    require(second.succeeded() && second.path.endsWith(QStringLiteral("Auto_2.md")),
            "quick save must preserve existing primary files too");
}

void emptyAndFailedPairLeaveNoPartialOutput() {
    QTemporaryDir directory;
    require(directory.isValid(), "failure save directory unavailable");
    const QString primary = directory.filePath(QStringLiteral("result.html"));
    const QString companion = directory.filePath(QStringLiteral("result.txt"));
    const auto empty = ScreenshotRecognitionFileExport::saveToPath(
        {ScreenshotRecognitionFileKind::Html, {}}, primary);
    require(!empty.succeeded() && !QFileInfo::exists(primary),
            "an empty recognition result must not create a file");
    write(primary, QByteArrayLiteral("original"));
    require(QDir().mkpath(companion), "failure companion directory unavailable");
    const auto failed = ScreenshotRecognitionFileExport::saveToPath(
        {ScreenshotRecognitionFileKind::Html, QStringLiteral("replacement")}, primary, true);
    require(!failed.succeeded() && read(primary) == QByteArrayLiteral("original") &&
                QFileInfo(companion).isDir(),
            "a pair publication failure must preserve prior outputs");
}

void longQuickSaveNamesPreservePairsAndCollisions() {
    QTemporaryDir directory;
    require(directory.isValid(), "long recognition filename directory unavailable");
    for (const auto kind :
         {ScreenshotRecognitionFileKind::Markdown, ScreenshotRecognitionFileKind::Html}) {
        const ScreenshotRecognitionFileSnapshot snapshot{kind, QStringLiteral("source")};
        const QString base = QString::fromUtf8("文😀").repeated(100);
        for (int index = 0; index < 2; ++index) {
            const auto result =
                ScreenshotRecognitionFileExport::quickSave(snapshot, directory.path(), base);
            require(result.succeeded(), "long recognition names and their companions must save");
            if (index != 0)
                require(QFileInfo(result.path).completeBaseName().endsWith(QStringLiteral("_1")),
                        "long recognition names must retain the collision index");
            for (const QString& path :
                 ScreenshotRecognitionFileExport::outputPaths(result.path, kind)) {
                const QString name = QFileInfo(path).fileName();
                require(read(path) == QByteArrayLiteral("source") && name.toUtf8().size() <= 255 &&
                            QString::fromUtf8(name.toUtf8()) == name,
                        "recognition pairs must fit the component limit and preserve Unicode");
            }
        }
        require(QDir(directory.path()).removeRecursively() && QDir().mkpath(directory.path()),
                "reset the recognition pair fixture");
    }
}

void manualOverwriteConfirmsAndReplacesBothFiles() {
    QTemporaryDir directory;
    require(directory.isValid(), "overwrite directory unavailable");
    const QString primary = directory.filePath(QStringLiteral("result.md"));
    const QString companion = directory.filePath(QStringLiteral("result.txt"));
    write(primary, QByteArrayLiteral("old primary"));
    write(companion, QByteArrayLiteral("old companion"));
    const QStringList paths = ScreenshotRecognitionFileExport::outputPaths(
        primary, ScreenshotRecognitionFileKind::Markdown);
    const auto confirm = [&](QMessageBox::StandardButton answer) {
        QTimer timer;
        timer.setInterval(10);
        QObject::connect(&timer, &QTimer::timeout, &timer, [answer]() {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (auto* box = qobject_cast<QMessageBox*>(widget); box && box->isVisible())
                    box->button(answer)->click();
            }
        });
        timer.start();
        const bool accepted = ScreenshotRecognitionFileExport::confirmOverwrite(nullptr, paths);
        timer.stop();
        return accepted;
    };
    require(!confirm(QMessageBox::No) && read(primary) == QByteArrayLiteral("old primary") &&
                read(companion) == QByteArrayLiteral("old companion"),
            "declining overwrite must preserve both existing files");
    require(confirm(QMessageBox::Yes), "manual overwrite requires explicit confirmation");
    const auto saved = ScreenshotRecognitionFileExport::saveToPath(
        {ScreenshotRecognitionFileKind::Markdown, QStringLiteral("# new")}, primary, true);
    require(saved.succeeded() && read(primary) == QByteArrayLiteral("# new") &&
                read(companion) == QByteArrayLiteral("# new"),
            "confirmed overwrite must replace both members from one source snapshot");
}

void overwriteButtonsFollowSelectedLanguage() {
    QTemporaryDir directory;
    require(directory.isValid(), "overwrite translation directory unavailable");
    const QString existing = directory.filePath(QStringLiteral("result.txt"));
    write(existing, QByteArrayLiteral("old"));
    struct Case {
        const char* locale;
        const char* replace;
        const char* cancel;
        const char* workbookFilter;
        const char* sheetName;
    };
    for (const Case& test : {Case{"en_US", "Replace", "Cancel", "Excel workbook (*.xlsx)", "Table"},
                             Case{"zh_CN", "替换", "取消", "Excel 工作簿 (*.xlsx)", "表格"},
                             Case{"zh_TW", "取代", "取消", "Excel 活頁簿 (*.xlsx)", "表格"}}) {
        QTranslator translator;
        require(translator.load(
                    QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(test.locale))),
                "overwrite confirmation catalog did not load");
        QCoreApplication::installTranslator(&translator);
        require(ScreenshotRecognitionFileExport::dialogFilter(
                    ScreenshotRecognitionFileKind::Table) == QString::fromUtf8(test.workbookFilter),
                "the Excel file filter must follow the selected language");
        const auto workbook = ScreenshotRecognitionFileExport::saveToPath(
            tableSnapshot(),
            directory.filePath(QString::fromLatin1(test.locale) + QStringLiteral(".xlsx")));
        require(workbook.succeeded(), "localized workbook could not be saved");
        const auto package = recognition_workbook_test::parts(workbook.path);
        QXmlStreamReader xml(package.value(QStringLiteral("xl/workbook.xml")));
        QString sheetName;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == u"sheet")
                sheetName = xml.attributes().value(u"name").toString();
        }
        require(!xml.hasError() && sheetName == QString::fromUtf8(test.sheetName),
                "the worksheet name must follow the selected language");
        bool inspected = false;
        bool labelsMatch = false;
        QTimer::singleShot(0, qApp, [&] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* dialog = qobject_cast<QMessageBox*>(widget);
                if (dialog == nullptr || !dialog->isVisible())
                    continue;
                inspected = true;
                labelsMatch =
                    dialog->testOption(QMessageBox::Option::DontUseNativeDialog) &&
                    dialog->button(QMessageBox::Yes)->text() == QString::fromUtf8(test.replace) &&
                    dialog->button(QMessageBox::No)->text() == QString::fromUtf8(test.cancel) &&
                    dialog->defaultButton() == dialog->button(QMessageBox::No);
                dialog->button(QMessageBox::No)->click();
                return;
            }
        });
        require(!ScreenshotRecognitionFileExport::confirmOverwrite(nullptr, {existing}),
                "canceling overwrite must decline replacement");
        require(inspected && labelsMatch, "overwrite buttons must use the selected app language");
        QCoreApplication::removeTranslator(&translator);
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        savesRealWorkbookWithLiteralCellsAndSpans();
        workbookSaveHandlesCollisionsOverwriteAndFailures();
        savesConversionPairsWithExactSource();
        savesQrValuesAsOneTextFile();
        savesLatexAsOneTextFile();
        quickSaveAvoidsEitherPairCollision();
        longQuickSaveNamesPreservePairsAndCollisions();
        emptyAndFailedPairLeaveNoPartialOutput();
        manualOverwriteConfirmsAndReplacesBothFiles();
        overwriteButtonsFollowSelectedLanguage();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
