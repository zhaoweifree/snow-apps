// Included by screenshot_clipboard_content_tests.cpp beside its require helper.
void pinnedColorSwatchContentTests() {
    const auto green = parsePinnedColorSwatch(QStringLiteral("#7DD353"));
    require(green && green->color == QColor(125, 211, 83), "parse reference HEX");
    require(green->clipboard[0] == QStringLiteral("#7DD353") &&
                green->clipboard[1] == QStringLiteral("rgb(125, 211, 83)") &&
                green->clipboard[2] == QStringLiteral("hsl(100.3, 59.3%, 57.6%)"),
            "reference rows copy canonical CSS");
    for (const auto& text : {QStringLiteral(" #7dd353 \n"), QStringLiteral("7DD353"),
                             QStringLiteral("RGB(125, 211, 83)"), QStringLiteral("rgb(125 211 83)"),
                             QStringLiteral("hsl(100.3deg, 59.3%, 57.6%)"),
                             QStringLiteral("hsl(100.3 59.3% 57.6%)")}) {
        const auto value = parsePinnedColorSwatch(text);
        require(value && value->color == green->color, "equivalent color syntax");
    }
    for (const auto& text :
         {QStringLiteral("#F008"), QStringLiteral("#FF000088"), QStringLiteral("FF000088"),
          QStringLiteral("rgba(255,0,0,0.533)"), QStringLiteral("rgb(100% 0% 0% / 53.333%)"),
          QStringLiteral("hsla(-360deg,100%,50%,0.533)"),
          QStringLiteral("hsl(720 100% 50% / 53.333%)")}) {
        const auto value = parsePinnedColorSwatch(text);
        require(value && value->color == QColor(255, 0, 0, 136), "CSS alpha-last syntax");
        require(value->clipboard[0] == QStringLiteral("#FF000088") &&
                    value->clipboard[1] == QStringLiteral("rgba(255, 0, 0, 0.533)") &&
                    value->clipboard[2] == QStringLiteral("hsla(0.0, 100.0%, 50.0%, 0.533)"),
                "alpha copies retain eight-bit precision");
    }
    require(parsePinnedColorSwatch(QStringLiteral("#abc"))->clipboard[0] ==
                QStringLiteral("#AABBCC"),
            "expand short HEX");
    require(parsePinnedColorSwatch(QStringLiteral("rgb(100% 0 0)"))->color == QColor(Qt::red),
            "modern RGB permits mixed channel units");
    require(parsePinnedColorSwatch(QStringLiteral("rgb(50%, 50%, 50%)"))->color ==
                QColor(128, 128, 128),
            "percentage midpoints round to the nearest eight-bit channel");
    require(!parsePinnedColorSwatch(QStringLiteral("hsl(-+1, 50%, 50%)")) &&
                !parsePinnedColorSwatch(QStringLiteral("rgb(1,2,3,)")),
            "reject malformed signs and empty alpha");
    require(parsePinnedColorSwatch(QStringLiteral("#0000"))->color.alpha() == 0 &&
                parsePinnedColorSwatch(QStringLiteral("#FFFFFF"))->clipboard[2] ==
                    QStringLiteral("hsl(0.0, 0.0%, 100.0%)"),
            "transparent and achromatic colors");
    for (const auto& text :
         {QStringLiteral("red"), QStringLiteral("abc"), QStringLiteral("value: #7DD353"),
          QStringLiteral("#7DD353\n#fff"), QStringLiteral("#12345"), QStringLiteral("#123456789"),
          QStringLiteral("125, 211, 83"), QStringLiteral("rgb(256,0,0)"),
          QStringLiteral("rgb(-1,0,0)"), QStringLiteral("rgb(10%,2,3)"),
          QStringLiteral("rgba(1,2,3,2)"), QStringLiteral("rgb(1 2 3 /)"),
          QStringLiteral("hsl(10,20,30)"), QStringLiteral("hsl(0,101%,50%)"),
          QStringLiteral("rgb(1,2,3 / .5)"), QStringLiteral("rgb(nan,0,0)"),
          QStringLiteral("rgb(1,2,3) trailing")})
        require(!parsePinnedColorSwatch(text), "invalid colors remain text");

    PinnedColorSwatchStyle style;
    style.font = QApplication::font();
    style.colorLabel = QStringLiteral("Color:");
    style.labels = {QStringLiteral("HEX:"), QStringLiteral("RGB:"), QStringLiteral("HSL:")};
    style.rgbaLabel = QStringLiteral("RGBA:");
    style.hslaLabel = QStringLiteral("HSLA:");
    ScreenshotClipboardContentSnapshot snapshot;
    snapshot.text = QStringLiteral("#7DD353");
    auto ordinary = ScreenshotClipboardContentReader::decode(snapshot);
    require(ordinary && (!ordinary->originalContent.textStyle ||
                         !ordinary->originalContent.textStyle->colorSwatch),
            "generic imports preserve ordinary text");
    snapshot.colorSwatchStyle = style;
    for (const qreal dpr : {1.0, 1.25, 2.0}) {
        snapshot.devicePixelRatio = dpr;
        auto card = ScreenshotClipboardContentReader::decode(snapshot);
        require(card && card->isFormattedText() && card->originalContent.textStyle->colorSwatch &&
                    card->originalContent.textStyle->fontSize == 16 &&
                    card->formattedDocument->defaultFont().pixelSize() == 16 &&
                    card->originalContent.textStyle->colorSwatchSize == QSize(342, 210) &&
                    card->image.size() == QSize(qCeil(342 * dpr), qCeil(210 * dpr)) &&
                    card->originalContent.text == snapshot.text &&
                    card->plainText.contains(QStringLiteral("125, 211, 83")),
                "pin-specific cards preserve original text, selection and DPR");
        const auto restored = ScreenshotClipboardContentReader::renderOriginalText(
            card->originalContent, dpr, {}, {}, style);
        require(restored && restored->image == card->image, "restore uses identical layout");
        const auto rendering = renderPinnedColorSwatch(*green, style, dpr);
        for (const auto& rect : rendering.copyRects)
            require(QRectF(0, 0, 342, 210).contains(rect), "copy targets fit compact card bounds");
        auto previousLayout = card->originalContent;
        previousLayout.textStyle->colorSwatchSize = QSize(470, 288);
        previousLayout.textStyle->maximumWidth = 470;
        const auto legacy = ScreenshotClipboardContentReader::renderOriginalText(
            previousLayout, dpr, {}, {}, style);
        require(legacy && legacy->image.size() == QSize(qCeil(470 * dpr), qCeil(288 * dpr)),
                "restoring older cards preserves their saved layout dimensions");
    }
    snapshot.html = QStringLiteral("<b>#7DD353</b>");
    auto html = ScreenshotClipboardContentReader::decode(snapshot);
    require(html &&
                (!html->originalContent.textStyle || !html->originalContent.textStyle->colorSwatch),
            "HTML pins keep source styling");
    snapshot.html.clear();
    snapshot.textFormat = snow_shot::storage::PinnedTextFormat::Markdown;
    auto markdown = ScreenshotClipboardContentReader::decode(snapshot);
    require(markdown && (!markdown->originalContent.textStyle ||
                         !markdown->originalContent.textStyle->colorSwatch),
            "Markdown pins keep source styling");
    snapshot.textFormat = snow_shot::storage::PinnedTextFormat::PlainText;
    snapshot.detachedImage = QImage(2, 2, QImage::Format_RGB32);
    snapshot.detachedImage.fill(Qt::red);
    auto image = ScreenshotClipboardContentReader::decode(snapshot);
    require(image && !image->isFormattedText() && image->image.size() == QSize(2, 2),
            "images retain MIME precedence");
    snapshot.detachedImage = {};
    require(!ScreenshotClipboardContentReader::decode(snapshot, [] { return true; }),
            "cancelled color decoding stops");
    require(!ScreenshotClipboardContentReader::decode(snapshot, {},
                                                      [](qint64 bytes) { return bytes < 1024; }),
            "allocation rejection precedes card rasterization");
    const auto previewDirectory = qEnvironmentVariable("SNOW_COLOR_SWATCH_PREVIEW_DIR");
    if (!previewDirectory.isEmpty()) {
        QDir().mkpath(previewDirectory);
        for (const bool dark : {false, true}) {
            style.background = dark ? QColor(20, 20, 20) : QColor(Qt::white);
            style.foreground = dark ? QColor(240, 240, 240) : QColor(38, 38, 38);
            style.secondary = dark ? QColor(160, 160, 160) : QColor(140, 140, 140);
            style.link = dark ? QColor(105, 177, 255) : QColor(22, 119, 255);
            for (const bool alpha : {false, true}) {
                const auto values =
                    alpha ? parsePinnedColorSwatch(QStringLiteral("#7DD35380")) : green;
                for (const qreal dpr : {1.0, 1.25}) {
                    const auto rendered = renderPinnedColorSwatch(*values, style, dpr);
                    require(rendered.image.save(QDir(previewDirectory)
                                                    .filePath(QStringLiteral("card-%1-%2-%3.png")
                                                                  .arg(dark ? "dark" : "light",
                                                                       alpha ? "alpha" : "opaque")
                                                                  .arg(qRound(dpr * 100)))),
                            "save visual QA card");
                }
            }
        }
    }
}
