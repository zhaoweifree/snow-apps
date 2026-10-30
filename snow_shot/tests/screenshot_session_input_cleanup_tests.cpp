#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotoverlayinputhandler.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QCoreApplication>
#include <QPainterPath>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

// Resource lifetime is otherwise invisible after the public model has been reset.
struct ScreenshotSessionInputCleanupTestAccess {
    static qsizetype pointCapacity(const ScreenshotOverlayInputHandler& input) {
        return input.m_regionPoints.capacity();
    }

    static bool previewPending(const ScreenshotOverlayInputHandler& input) {
        return input.m_regionPreviewTimer.isActive();
    }

    static bool released(const ScreenshotOverlayInputHandler& input) {
        return input.m_regionPoints.isEmpty() && input.m_regionPoints.capacity() == 0 &&
               input.m_freehandRawStart == 0 && !input.m_regionPreviewTimer.isActive() &&
               input.m_pendingRegionPointer.isNull() && !input.m_pendingRegionEdge &&
               !input.m_freehandPressed && !input.m_consumeRegionRelease &&
               !input.m_consumeEffectRelease && !input.m_externalDragActive;
    }

    static bool filterHasNoOutput(ScreenshotOverlayInputHandler& input) {
        return input.m_freehandFilter.takePoints(true).isEmpty();
    }
};

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct InputFixture {
    ScreenshotCaptureState capture;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligent;
    ScreenshotGeometryMapper geometry;
    ScreenshotDisplaySession displays;
    int updates = 0;
    int scrollingPauses = 0;
    int scrollingResumes = 0;
    ScreenshotOverlayInputHandler input{
        {capture, interaction, selection, intelligent, geometry, displays, actions()}};

    ScreenshotOverlayInputActions actions() {
        ScreenshotOverlayInputActions result;
        result.updateOverlayState = [this] { ++updates; };
        result.pauseScrollingCapture = [this] { ++scrollingPauses; };
        result.resumeScrollingCapture = [this] { ++scrollingResumes; };
        return result;
    }

    void beginDraft(ScreenshotRegionType type) {
        interaction.beginCapture();
        selection.setRegionType(type);
        input.handleMousePress(nullptr, QPointF(20, 20));
        if (type == ScreenshotRegionType::Freehand) {
            for (int i = 1; i <= 48; ++i)
                input.handleMouseMove(nullptr, QPointF(20 + i, 20 + (i % 7)));
            QCoreApplication::processEvents();
        } else {
            for (int i = 1; i <= 12; ++i)
                input.handleMousePress(nullptr, QPointF(20 + i * 3, 20 + (i % 3) * 8));
        }
        input.handleMouseMove(nullptr, QPointF(100, 80));
        require(selection.constructionActive(), "pointer input must create a custom-region draft");
        require(ScreenshotSessionInputCleanupTestAccess::pointCapacity(input) > 0,
                "draft construction must allocate point storage");
        require(ScreenshotSessionInputCleanupTestAccess::previewPending(input),
                "pointer movement must queue a custom-region preview");
    }

    void requireReleased() {
        require(ScreenshotSessionInputCleanupTestAccess::released(input),
                "capture termination must release all custom-region input storage and state");
        require(ScreenshotSessionInputCleanupTestAccess::filterHasNoOutput(input),
                "capture termination must discard pending native freehand input");
        const int before = updates;
        QCoreApplication::processEvents();
        require(updates == before, "terminated captures must not deliver queued region previews");
    }
};

void captureTerminationReleasesActiveCustomRegionDrafts() {
    for (const auto type : {ScreenshotRegionType::Polyline, ScreenshotRegionType::Curve,
                            ScreenshotRegionType::Freehand}) {
        for (const bool resetModelFirst : {false, true}) {
            InputFixture fixture;
            for (int cycle = 0; cycle < 3; ++cycle) {
                fixture.beginDraft(type);
                const auto vertices = fixture.selection.draftVertices();
                const auto pointCapacity =
                    ScreenshotSessionInputCleanupTestAccess::pointCapacity(fixture.input);
                fixture.input.resetTransientShortcuts();
                require(fixture.selection.constructionActive() &&
                            fixture.selection.draftVertices() == vertices &&
                            ScreenshotSessionInputCleanupTestAccess::pointCapacity(fixture.input) ==
                                pointCapacity,
                        "resetting shortcuts within a capture must preserve its active draft");
                if (resetModelFirst)
                    fixture.selection.reset();
                fixture.input.endCaptureSession();
                fixture.selection.reset();
                fixture.interaction.reset();
                fixture.requireReleased();
                fixture.input.endCaptureSession();
                fixture.requireReleased();
            }
        }
    }
}

void captureTerminationReleasesCompletedFreehandInput() {
    InputFixture fixture;
    fixture.beginDraft(ScreenshotRegionType::Freehand);
    fixture.input.handleMouseMove(nullptr, QPointF(100, 140));
    fixture.input.handleMouseMove(nullptr, QPointF(20, 140));
    fixture.input.handleMouseRelease(nullptr, QPointF(20, 20));
    require(!fixture.selection.constructionActive() && fixture.selection.hasPixelSelection(),
            "freehand release must commit a filled custom selection");
    require(ScreenshotSessionInputCleanupTestAccess::pointCapacity(fixture.input) > 0,
            "completed input must exercise retained construction storage");
    fixture.input.endCaptureSession();
    fixture.selection.reset();
    fixture.interaction.reset();
    fixture.requireReleased();
}

void captureTerminationClearsExternalInputState() {
    InputFixture fixture;
    fixture.input.setExternalDragActive(true);
    fixture.input.armCanvasColorSampling();
    fixture.input.endCaptureSession();
    require(!fixture.input.canvasColorSamplingActive(),
            "capture termination must disarm the canvas color sampler");
    fixture.requireReleased();
}

void captureTerminationDoesNotResumeScrollingCapture() {
    InputFixture fixture;
    fixture.selection.setSelectionRect(QRectF(20, 20, 100, 100));
    fixture.interaction.enterScrollingCapture();
    require(fixture.input.beginSelectionResizeAtCanvasPosition(QPointF(20, 20)),
            "scrolling capture must permit border resize");
    require(fixture.scrollingPauses == 1 && fixture.scrollingResumes == 0,
            "border resize must pause scrolling until the live gesture completes");
    fixture.input.endCaptureSession();
    require(fixture.scrollingResumes == 0,
            "session termination must not resume an interrupted scrolling capture");
    fixture.requireReleased();
}

void sessionModelResetReleasesRegionBuffers() {
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(0, 0, 10, 10));
    for (int i = 0; i < 24; ++i) {
        selection.beginRegionOperation(ScreenshotSelectionModel::RegionOperation::Add);
        selection.setSelectionRect(QRectF(i * 12 + 12, 0, 10, 10));
        selection.commitRegionOperation();
    }
    require(selection.createdRegionCount() > 20 && selection.regionStorageBytesForTesting() > 0,
            "compound selection construction must allocate an editable region collection");
    QVector<QPointF> vertices;
    for (int i = 0; i < 256; ++i)
        vertices.append(QPointF(i, i % 2));
    QPainterPath path;
    path.addRect(QRectF(0, 0, 256, 16));
    selection.setDraftRegion(
        ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Polyline), vertices);
    require(selection.draftVertices().capacity() >= vertices.size(),
            "custom selection construction must allocate draft vertex storage");
    selection.reset();
    require(selection.draftVertices().isEmpty() && selection.draftVertices().capacity() == 0,
            "session model reset must release draft vertex capacity");
    require(selection.createdRegionCount() == 0 && selection.regionStorageBytesForTesting() == 0,
            "session model reset must release the editable region collection capacity");
    require(!selection.constructionActive() && selection.selectionRegion().isEmpty(),
            "session model reset must clear draft and committed geometry");
    const auto revision = selection.changeRevision();
    selection.reset();
    require(selection.changeRevision() == revision,
            "repeated session cleanup must preserve the empty model revision");
}
} // namespace

void runScreenshotSessionInputCleanupTests() {
    QTemporaryDir directory;
    require(directory.isValid(), "session input cleanup tests require isolated storage");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    const snow_shot::storage::StorageInitializationOptions options{
        directory.filePath(QStringLiteral("bin")), directory.filePath(QStringLiteral("data")),
        60000};
    require(storage.initialize(options).success, "session input test storage must initialize");
    const auto shutdown = qScopeGuard([&storage] { storage.shutdown(); });

    captureTerminationReleasesActiveCustomRegionDrafts();
    captureTerminationReleasesCompletedFreehandInput();
    captureTerminationClearsExternalInputState();
    captureTerminationDoesNotResumeScrollingCapture();
    sessionModelResetReleasesRegionBuffers();
}
