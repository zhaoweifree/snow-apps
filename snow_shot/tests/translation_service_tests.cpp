#include "translation_test_support.h"
#include "snow_shot/translation/translationservice.h"
#include "snow_shot/translation/translationlanguagedetector.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/translation/translationlanguages.h"
#include <QTemporaryDir>
#include <memory>

using namespace translation_tests;
using namespace snow_shot::translation;
using snow_shot::storage::ConfigurationStore;

namespace {
void discoverySharesRequestsAndCancellation(const QString& directory) {
    Server server;
    server.holdModels = true;
    SnowShotApiClient client(server.url());
    auto settings = std::make_unique<ConfigurationStore>(
        directory + QStringLiteral("/shared-discovery.json"), true, true, 60000);
    QPointer<TranslationService> service =
        &TranslationService::forClient(client, *settings, QLocale::English);
    const QString locale = QLocale(QLocale::English).name();
    QObject recognitionReceiver;
    int cancelledCompletions = 0;
    const auto recognition = client.ensureChatModels(locale, &recognitionReceiver,
                                                     [&](auto) { ++cancelledCompletions; });
    require(recognition != 0, "recognition starts shared model discovery");
    service->refreshModels();
    service->refreshModels();
    waitUntil([&] { return server.modelRequests == 1; }, "translation joins recognition discovery");
    client.cancel(recognition);
    server.respondModels();
    waitUntil([&] { return !service->loadingModels(); },
              "translation finishes after recognition subscriber cancellation");
    require(server.modelRequests == 1 && cancelledCompletions == 0 &&
                service->preferences().modelId == QStringLiteral("general") &&
                service->models() == client.cachedChatModels(),
            "translation and recognition share one independently cancellable catalog request");

    service->refreshModels();
    waitUntil([&] { return !service->loadingModels(); }, "translation reuses the cached catalog");
    require(server.modelRequests == 1, "ordinary translation discovery avoids another request");

    server.models =
        QJsonArray{QJsonObject{{QStringLiteral("model"), QStringLiteral("updated")},
                               {QStringLiteral("name"), QStringLiteral("Updated model")},
                               {QStringLiteral("supports_vision"), true}}};
    const auto refresh = client.ensureChatModels(
        locale, &recognitionReceiver, [&](auto) { ++cancelledCompletions; },
        SnowShotApiClient::ChatModelsCachePolicy::Refresh);
    require(refresh != 0, "recognition explicitly refreshes the cached catalog");
    service->refreshModels();
    require(service->loadingModels(), "ordinary translation discovery joins an in-flight refresh");
    waitUntil([&] { return server.modelRequests == 2; }, "refresh sends one shared request");
    client.cancel(refresh);
    server.respondModels();
    waitUntil([&] { return !service->loadingModels(); }, "translation receives refreshed models");
    require(server.modelRequests == 2 && cancelledCompletions == 0 &&
                service->preferences().modelId == QStringLiteral("updated"),
            "shared refresh updates translation selection after the other subscriber cancels");

    service->refreshModels(true);
    int refreshCompletions = 0;
    require(client.ensureChatModels(
                locale, &recognitionReceiver,
                [&](SnowShotChatModelsResult result) {
                    require(result.succeeded() &&
                                result.models.first().id == QStringLiteral("updated"),
                            "remaining recognition subscriber receives refreshed models");
                    ++refreshCompletions;
                },
                SnowShotApiClient::ChatModelsCachePolicy::Refresh) != 0,
            "recognition joins explicit translation refresh");
    waitUntil([&] { return server.modelRequests == 3; },
              "explicit translation refresh bypasses the warm cache once");
    settings.reset();
    require(service == nullptr, "settings destruction removes the translation subscriber");
    server.respondModels();
    waitUntil([&] { return refreshCompletions == 1; },
              "recognition discovery survives translation subscriber destruction");
    require(server.modelRequests == 3 && cancelledCompletions == 0,
            "explicit refresh and cancellation preserve a single catalog owner");
}

void secondaryTargetsPersistAndReachEveryUnit(const QString& directory) {
    Server server;
    SnowShotApiClient client(server.url());
    ConfigurationStore settings(directory + QStringLiteral("/secondary.json"), true, true, 60000);
    require(settings.setValue(QStringLiteral("screenshot_translation/target_language"),
                              QStringLiteral("zh-Hant")),
            "save an existing primary target");
    auto& service = TranslationService::forClient(client, settings, QLocale::English);
    require(service.preferences().targetLanguage == QStringLiteral("zh-Hant") &&
                service.preferences().secondaryTargetLanguage == QStringLiteral("en"),
            "new secondary default preserves an existing primary selection");
    for (const auto& language : translationLanguages()) {
        const QString code = QString::fromLatin1(language.code);
        require(
            settings.setValue(QStringLiteral("screenshot_translation/source_language"), code) &&
                settings.setValue(QStringLiteral("screenshot_translation/target_language"), code) &&
                settings.setValue(
                    QStringLiteral("screenshot_translation/secondary_target_language"), code),
            "primary and secondary accept the entire language catalog, including identical "
            "targets");
    }
    require(!settings.setValue(QStringLiteral("screenshot_translation/secondary_target_language"),
                               QStringLiteral("auto")),
            "secondary target cannot auto-detect");
    auto preferences = service.preferences();
    preferences.sourceLanguage = QStringLiteral("auto");
    preferences.targetLanguage = QStringLiteral("zh-Hans");
    preferences.secondaryTargetLanguage = QStringLiteral("ja");
    preferences.modelId = QStringLiteral("general");
    require(service.savePreferences(preferences), "save shared secondary preferences");
    QObject owner;
    const QStringList texts{QStringLiteral("The complete source text is written in English."),
                            QStringLiteral("Hello")};
    auto* job = service.createJob(texts, &owner);
    job->start();
    waitUntil([&] { return server.streams.size() == 2; }, "each input block has its own request");
    for (const auto& stream : server.streams) {
        const auto messages = stream.body.value(QStringLiteral("messages")).toArray();
        const auto prompt = messages.first().toObject().value(QStringLiteral("content")).toString();
        require(prompt.contains(QStringLiteral("Target language: zh-Hans")) &&
                    !prompt.contains(QStringLiteral("Secondary target language")),
                "every unit receives the same concrete primary target for unmatched source text");
    }
    job->cancel();
    require(settings.flushNow().success, "persist secondary preference before reopening");
    ConfigurationStore reopened(directory + QStringLiteral("/secondary.json"), true, true, 60000);
    require(reopened.value(QStringLiteral("screenshot_translation/secondary_target_language")) ==
                QStringLiteral("ja"),
            "secondary preference survives storage reload");
}

void detectionIsDeferredUntilRoutingNeedsIt(const QString& directory) {
    Server server;
    SnowShotApiClient client(server.url());
    ConfigurationStore settings(directory + QStringLiteral("/lazy-detection.json"), true, true,
                                60000);
    const QStringList texts{QStringLiteral("Bonjour"),
                            QStringLiteral("The remaining source text is written in English.")};
    int detectionCalls = 0;
    auto& service = TranslationService::forClient(
        client, settings, QLocale::English, [&](const QString& input) {
            ++detectionCalls;
            require(input == texts.join(u'\n'), "deferred detection keeps the complete input");
            return std::optional<DetectedTranslationLanguage>{{QStringLiteral("eng"), 0.8}};
        });
    auto preferences = service.preferences();
    preferences.modelId = QStringLiteral("general");
    preferences.targetLanguage = QStringLiteral("en");
    preferences.secondaryTargetLanguage = QStringLiteral("en");
    require(service.savePreferences(preferences), "configure identical translation targets");
    QObject owner;
    auto* job = service.createJob(texts, &owner);
    const auto verifyTarget = [&](int count, const QString& target) {
        waitUntil([&] { return server.streams.size() == count; }, "send lazy-routing units");
        for (qsizetype index = count - texts.size(); index < count; ++index) {
            const auto prompt = server.streams[index]
                                    .body.value(QStringLiteral("messages"))
                                    .toArray()
                                    .first()
                                    .toObject()
                                    .value(QStringLiteral("content"))
                                    .toString();
            require(prompt.contains(QStringLiteral("Target language: %1\n").arg(target)),
                    "lazy routing sends the concrete target to every unit");
        }
    };
    job->start();
    verifyTarget(2, QStringLiteral("en"));
    require(detectionCalls == 0, "identical targets need no language detection");
    job->cancel();
    preferences.secondaryTargetLanguage = QStringLiteral("fr");
    require(service.savePreferences(preferences), "enable target switching before retry");
    job->retry();
    verifyTarget(4, QStringLiteral("fr"));
    require(detectionCalls == 1, "routing first detects when a retry enables switching");
    job->cancel();
    preferences.secondaryTargetLanguage = QStringLiteral("en");
    require(service.savePreferences(preferences), "disable target switching before retry");
    job->retry();
    verifyTarget(6, QStringLiteral("en"));
    job->cancel();
    preferences.secondaryTargetLanguage = QStringLiteral("fr");
    require(service.savePreferences(preferences), "re-enable target switching before retry");
    job->retry();
    verifyTarget(8, QStringLiteral("fr"));
    require(detectionCalls == 1, "re-enabled routing reuses the original detection");
    job->cancel();

    preferences.secondaryTargetLanguage.clear();
    auto* fixed = service.createJob(texts, preferences, &owner);
    fixed->start();
    verifyTarget(10, QStringLiteral("en"));
    require(detectionCalls == 1, "an absent secondary target needs no detection");
    fixed->cancel();
    auto* empty = service.createJob({QString(), QStringLiteral("\t\n")}, &owner);
    empty->start();
    empty->retry();
    require(empty->state() == TranslationJob::State::Completed && detectionCalls == 1 &&
                server.streams.size() == 10,
            "empty jobs complete without detection or provider requests");
}

void completeInputRoutingReachesEveryProviderAndRetries(const QString& directory) {
    const QStringList texts{QStringLiteral("Bonjour"),
                            QStringLiteral("The rest of the complete input is written in English, "
                                           "and both output blocks must use the same target.")};
    for (const auto& provider : {QStringLiteral("general"), QStringLiteral("specialist"),
                                 QStringLiteral("custom"), QStringLiteral("native")}) {
        Server builtIn;
        Server external;
        external.streamPath = provider == QStringLiteral("native")
                                  ? QByteArrayLiteral("/v2/translate")
                                  : QByteArrayLiteral("/v1/chat/completions");
        SnowShotApiClient client(builtIn.url());
        ConfigurationStore settings(directory + QStringLiteral("/routing-%1.json").arg(provider),
                                    true, true, 60000);
        QString model = provider;
        if (provider == QStringLiteral("custom")) {
            const snow_shot::CustomAiModelConfiguration configuration{
                QStringLiteral("11111111-1111-4111-8111-111111111111"),
                QStringLiteral("Custom routing test"),
                external.url() + QStringLiteral("/v1"),
                QStringLiteral("test-key"),
                QStringLiteral("provider-model"),
                false};
            require(settings.setValue(QStringLiteral("api_configuration/custom_models"),
                                      snow_shot::customAiModelsToJson({configuration})),
                    "configure custom routing provider");
            model = configuration.selectionId();
        } else if (provider == QStringLiteral("native")) {
            const snow_shot::TextTranslationConfiguration configuration{
                QStringLiteral("22222222-2222-4222-8222-222222222222"),
                QStringLiteral("Native routing test"),
                QStringLiteral("deepl"),
                external.url() + QStringLiteral("/v2/translate"),
                QStringLiteral("test-key"),
                {},
                4};
            require(
                settings.setValue(QStringLiteral("api_configuration/text_translation"),
                                  snow_shot::textTranslationConfigurationsToJson({configuration})),
                "configure native routing provider");
            model = configuration.selectionId();
        }
        int detectionCalls = 0;
        auto& service = TranslationService::forClient(
            client, settings, QLocale::English, [&](const QString& input) {
                ++detectionCalls;
                require(input == texts.join(u'\n'),
                        "one detection call receives every complete original block together");
                return std::optional<DetectedTranslationLanguage>{{QStringLiteral("eng"), 0.8}};
            });
        auto preferences = service.preferences();
        preferences.sourceLanguage = QStringLiteral("ja");
        preferences.targetLanguage = QStringLiteral("en");
        preferences.secondaryTargetLanguage = QStringLiteral("fr");
        preferences.languageDetectionConfidenceThreshold = 60;
        preferences.modelId = model;
        require(service.savePreferences(preferences), "save routing preferences");
        Server& server =
            provider == QStringLiteral("custom") || provider == QStringLiteral("native") ? external
                                                                                         : builtIn;
        const auto verifyRequests = [&](int first, const QString& target) {
            waitUntil([&] { return server.streams.size() == first + texts.size(); },
                      "all original units reach the selected provider");
            QStringList transmittedTexts;
            for (int index = first; index < first + texts.size(); ++index) {
                const auto& body = server.streams[index].body;
                if (provider == QStringLiteral("native")) {
                    require(body.value(QStringLiteral("target_lang")) == target.toUpper() &&
                                body.value(QStringLiteral("source_lang")) == QStringLiteral("JA"),
                            "native provider receives resolved target and unchanged source hint");
                    transmittedTexts.append(
                        body.value(QStringLiteral("text")).toArray().first().toString());
                } else if (provider == QStringLiteral("specialist")) {
                    const auto options =
                        body.value(QStringLiteral("translation_options")).toObject();
                    require(options.value(QStringLiteral("target_lang")) == target &&
                                options.value(QStringLiteral("source_lang")) ==
                                    QStringLiteral("ja"),
                            "Qwen MT receives resolved target and unchanged source hint");
                    transmittedTexts.append(body.value(QStringLiteral("messages"))
                                                .toArray()
                                                .first()
                                                .toObject()
                                                .value(QStringLiteral("content"))
                                                .toString());
                } else {
                    const auto messages = body.value(QStringLiteral("messages")).toArray();
                    const auto prompt =
                        messages.first().toObject().value(QStringLiteral("content")).toString();
                    require(
                        prompt.contains(QStringLiteral("Source language: ja\nTarget language: %1\n")
                                            .arg(target)) &&
                            !prompt.contains(QStringLiteral("Secondary target language")),
                        "chat model receives resolved target without language routing");
                    transmittedTexts.append(
                        messages.last().toObject().value(QStringLiteral("content")).toString());
                }
            }
            transmittedTexts.sort();
            auto originalTexts = texts;
            originalTexts.sort();
            require(transmittedTexts == originalTexts,
                    "translation preserves each original unit while sharing the detected target");
        };
        QObject owner;
        auto* job = service.createJob(texts, &owner);
        job->start();
        verifyRequests(0, QStringLiteral("fr"));
        require(detectionCalls == 1, "initial routing detects complete input once");
        job->cancel();
        auto changed = preferences;
        changed.secondaryTargetLanguage = QStringLiteral("de");
        changed.languageDetectionConfidenceThreshold = 90;
        require(service.savePreferences(changed), "raise threshold before retry");
        job->retry();
        verifyRequests(2, QStringLiteral("en"));
        require(detectionCalls == 1 && job->preferences() == changed,
                "retry reapplies current threshold to the cached complete input detection");
        job->cancel();

        auto* fixed = service.createJob(texts, preferences, &owner);
        fixed->start();
        verifyRequests(4, QStringLiteral("fr"));
        require(detectionCalls == 2, "each separate job detects its complete input once");
        fixed->cancel();
        fixed->retry();
        verifyRequests(6, QStringLiteral("fr"));
        require(detectionCalls == 2 && fixed->preferences() == preferences,
                "fixed retry retains captured threshold and target preferences");
        fixed->cancel();
    }
}

void failedUnitRetryKeepsCompletedInputContext(const QString& directory) {
    Server server;
    SnowShotApiClient client(server.url());
    ConfigurationStore settings(directory + QStringLiteral("/routing-partial-retry.json"), true,
                                true, 60000);
    const QStringList texts{QStringLiteral("Bonjour"),
                            QStringLiteral("The completed block contains the English context "
                                           "that determines the target for the entire input.")};
    int detectionCalls = 0;
    auto& service = TranslationService::forClient(
        client, settings, QLocale::English, [&](const QString& input) {
            ++detectionCalls;
            return std::optional<DetectedTranslationLanguage>{
                {input == texts.join(u'\n') ? QStringLiteral("eng") : QStringLiteral("fra"), 0.8}};
        });
    auto preferences = service.preferences();
    preferences.targetLanguage = QStringLiteral("en");
    preferences.secondaryTargetLanguage = QStringLiteral("fr");
    preferences.modelId = QStringLiteral("general");
    require(service.savePreferences(preferences), "configure partial retry routing");
    QObject owner;
    auto* job = service.createJob(texts, &owner);
    job->start();
    waitUntil([&] { return server.streams.size() == 2; }, "send both original input blocks");
    const auto firstText = server.streams.first()
                               .body.value(QStringLiteral("messages"))
                               .toArray()
                               .last()
                               .toObject()
                               .value(QStringLiteral("content"))
                               .toString();
    const int completedStream = firstText == texts[1] ? 0 : 1;
    const int failedStream = 1 - completedStream;
    server.delta(completedStream, QStringLiteral("completed translation"));
    server.finish(completedStream);
    waitUntil([&] { return job->units()[1].state == TranslationJob::State::Completed; },
              "retain the context-bearing completed block");
    server.fail(failedStream);
    waitUntil([&] { return job->state() == TranslationJob::State::Failed; },
              "the other block fails independently");
    job->retry();
    waitUntil([&] { return server.streams.size() == 3; }, "retry only the failed block");
    const auto messages = server.streams.last().body.value(QStringLiteral("messages")).toArray();
    require(detectionCalls == 1 &&
                messages.first()
                    .toObject()
                    .value(QStringLiteral("content"))
                    .toString()
                    .contains(QStringLiteral("Target language: fr")) &&
                messages.last().toObject().value(QStringLiteral("content")) == texts[0] &&
                job->units()[1].text == QStringLiteral("completed translation"),
            "failed-unit retry uses cached full-input detection and preserves completed context");
    job->cancel();
}

void duplicateUnitsShareRequestsAndRetryByOriginalPosition(const QString& directory) {
    Server server;
    SnowShotApiClient client(server.url());
    ConfigurationStore settings(directory + QStringLiteral("/duplicate-units.json"), true, true,
                                60000);
    auto& service = TranslationService::forClient(client, settings, QLocale::English);
    QObject owner;
    const QString repeated = QStringLiteral("repeat");
    auto* job = service.createJob(
        {repeated, QStringLiteral(" repeat "), repeated, repeated, QString()}, &owner);
    QVector<int> changes(5);
    QObject::connect(job, &TranslationJob::unitChanged, &owner, [&](int index) {
        ++changes[index];
        require(job->units()[0].text == job->units()[2].text &&
                    job->units()[0].text == job->units()[3].text,
                "every duplicate position is updated before any index notification");
    });
    job->start();
    waitUntil([&] { return server.streams.size() == 2; },
              "exact duplicates share one request while whitespace differences stay distinct");
    const QString firstSource = server.streams[0]
                                    .body.value(QStringLiteral("messages"))
                                    .toArray()
                                    .last()
                                    .toObject()
                                    .value(QStringLiteral("content"))
                                    .toString();
    const int shared = firstSource == repeated ? 0 : 1;
    const int distinct = 1 - shared;
    server.delta(shared, QStringLiteral("par"));
    waitUntil([&] { return changes[0] == 1 && changes[2] == 1 && changes[3] == 1; },
              "one provider delta reaches every original duplicate index");
    server.delta(shared, QStringLiteral("tial"));
    waitUntil([&] { return changes[0] == 2 && changes[2] == 2 && changes[3] == 2; },
              "all duplicate positions receive subsequent streamed deltas");
    server.delta(distinct, QStringLiteral("distinct translation"));
    server.finish(distinct);
    waitUntil([&] { return job->units()[1].state == TranslationJob::State::Completed; },
              "the different source completes independently");
    server.fail(shared);
    waitUntil([&] { return job->state() == TranslationJob::State::Failed; },
              "failure of the shared request settles every duplicate");
    for (const int index : {0, 2, 3})
        require(job->units()[index].state == TranslationJob::State::Failed &&
                    job->units()[index].text == QStringLiteral("partial"),
                "shared failures retain partial text at every original position");
    job->retry();
    waitUntil([&] { return server.streams.size() == 3; },
              "retry sends one request for the failed duplicates");
    require(job->units()[1].state == TranslationJob::State::Completed &&
                job->units()[1].text == QStringLiteral("distinct translation"),
            "retry preserves the completed distinct source");
    server.delta(2, QStringLiteral("retried translation"));
    server.finish(2);
    waitUntil([&] { return job->state() == TranslationJob::State::Completed; },
              "the shared retry completes all original positions");
    for (const int index : {0, 2, 3})
        require(job->units()[index].state == TranslationJob::State::Completed &&
                    job->units()[index].text == QStringLiteral("retried translation") &&
                    changes[index] == 3,
                "retry replaces each duplicate's partial result and preserves index progress");
    require(changes[1] == 1 && changes[4] == 0 && job->units()[4].text.isEmpty() &&
                job->units()[4].state == TranslationJob::State::Completed,
            "completed and empty units need no extra request or delta notification");
}

void duplicateDeltaObserversMayCancelOrDestroyTheJob(const QString& directory) {
    Server server;
    SnowShotApiClient client(server.url());
    ConfigurationStore settings(directory + QStringLiteral("/duplicate-lifetime.json"), true, true,
                                60000);
    auto& service = TranslationService::forClient(client, settings, QLocale::English);
    std::unique_ptr<TranslationJob> job(
        service.createJob({QStringLiteral("same"), QStringLiteral("same")}, nullptr));
    QVector<int> notifications;
    bool cancelled = false;
    QObject::connect(job.get(), &TranslationJob::unitChanged, &client, [&](int index) {
        notifications.append(index);
        if (!cancelled) {
            cancelled = true;
            job->cancel();
        } else {
            job.reset();
        }
    });
    job->start();
    waitUntil([&] { return server.streams.size() == 1; }, "start the shared cancellation fixture");
    server.delta(0, QStringLiteral("partial"));
    waitUntil([&] { return server.disconnected(0); }, "cancellation aborts the shared request");
    require(notifications == QVector<int>{0} && job->state() == TranslationJob::State::Cancelled &&
                job->units()[0].state == TranslationJob::State::Cancelled &&
                job->units()[1].state == TranslationJob::State::Cancelled &&
                job->units()[0].text == QStringLiteral("partial") &&
                job->units()[1].text == QStringLiteral("partial"),
            "reentrant cancellation stops notifications while retaining a coherent unit snapshot");
    job->retry();
    waitUntil([&] { return server.streams.size() == 2; }, "retry creates one replacement request");
    server.delta(1, QStringLiteral("new output"));
    waitUntil([&] { return !job && server.disconnected(1); },
              "a duplicate delta observer may destroy its job safely");
    require(notifications == QVector<int>({0, 0}),
            "destroyed jobs publish no remaining duplicate-index notifications");
}

void terminalNotificationsRespectReentrantTransitions(const QString& directory) {
    Server server;
    SnowShotApiClient client(server.url());
    ConfigurationStore settings(directory + QStringLiteral("/terminal-transitions.json"), true,
                                true, 60000);
    auto& service = TranslationService::forClient(client, settings, QLocale::English);
    enum class Action { Retry, Cancel, Destroy };
    for (const auto action : {Action::Retry, Action::Cancel, Action::Destroy}) {
        std::unique_ptr<TranslationJob> job(service.createJob({QStringLiteral(" ")}, nullptr));
        int completions = 0;
        bool changed = false;
        QObject::connect(job.get(), &TranslationJob::finished, &client, [&] { ++completions; });
        QObject::connect(job.get(), &TranslationJob::stateChanged, &client, [&] {
            if (changed || job->state() != TranslationJob::State::Completed)
                return;
            changed = true;
            switch (action) {
            case Action::Retry:
                job->retry();
                break;
            case Action::Cancel:
                job->cancel();
                break;
            case Action::Destroy:
                job.reset();
                break;
            }
        });
        job->start();
        require(changed && completions == (action == Action::Retry ? 1 : 0),
                "empty completion publishes only the surviving transition after retry, cancel "
                "or destruction");
    }
    require(server.modelRequests == 0 && server.streams.isEmpty(),
            "reentrant empty jobs still avoid model discovery and requests");

    QObject owner;
    auto* job = service.createJob({QStringLiteral("source")}, &owner);
    int completions = 0;
    bool retried = false;
    QObject::connect(job, &TranslationJob::finished, &owner, [&] { ++completions; });
    QObject::connect(job, &TranslationJob::stateChanged, &owner, [&] {
        if (!retried && job->state() == TranslationJob::State::Completed) {
            retried = true;
            job->retry();
        }
    });
    job->start();
    waitUntil([&] { return server.streams.size() == 1; }, "start terminal streaming fixture");
    server.delta(0, QStringLiteral("translated"));
    server.finish(0);
    waitUntil([&] { return completions == 1; }, "publish the surviving completed generation");
    require(retried && job->state() == TranslationJob::State::Completed &&
                server.streams.size() == 1,
            "terminal pump retry retains completed output and publishes no stale completion");
}

void failedNotificationsRespectReentrantRetry(const QString& directory) {
    Server server;
    auto client = std::make_unique<SnowShotApiClient>(server.url());
    ConfigurationStore settings(directory + QStringLiteral("/terminal-failure.json"), true, true,
                                60000);
    auto& service = TranslationService::forClient(*client, settings, QLocale::English);
    QObject owner;
    auto* job = service.createJob({QStringLiteral("source")}, &owner);
    int completions = 0;
    bool retried = false;
    QObject::connect(job, &TranslationJob::finished, &owner, [&] { ++completions; });
    QObject::connect(job, &TranslationJob::stateChanged, &owner, [&] {
        if (!retried && job->state() == TranslationJob::State::Failed) {
            retried = true;
            job->retry();
        }
    });
    job->start();
    waitUntil([&] { return server.streams.size() == 1; }, "start service failure fixture");
    client.reset();
    require(retried && job->state() == TranslationJob::State::Failed && completions == 1,
            "service failure emits no obsolete completion after a listener retries and fails");
}

void invalidationNotificationsRespectReentrantRetry(const QString& directory) {
    Server server;
    SnowShotApiClient client(server.url());
    ConfigurationStore settings(directory + QStringLiteral("/terminal-invalidation.json"), true,
                                true, 60000);
    snow_shot::CustomAiModelConfiguration model{
        QStringLiteral("11111111-1111-4111-8111-111111111111"),
        QStringLiteral("Invalidation fixture"),
        server.url() + QStringLiteral("/v1"),
        QStringLiteral("test-key"),
        QStringLiteral("provider-model"),
        false};
    const QString key = QStringLiteral("api_configuration/custom_models");
    require(settings.setValue(key, snow_shot::customAiModelsToJson({model})),
            "configure terminal invalidation fixture");
    auto& service = TranslationService::forClient(client, settings, QLocale::English);
    QObject owner;
    auto* job = service.createJob({QStringLiteral(" ")}, &owner);
    int completions = 0;
    int invalidations = 0;
    bool retried = false;
    QObject::connect(job, &TranslationJob::finished, &owner, [&] { ++completions; });
    QObject::connect(job, &TranslationJob::invalidated, &owner, [&] { ++invalidations; });
    QObject::connect(job, &TranslationJob::stateChanged, &owner, [&] {
        if (!retried && job->state() == TranslationJob::State::Invalidated) {
            retried = true;
            job->retry();
        }
    });
    job->start();
    model.apiKey = QStringLiteral("replacement-key");
    require(settings.setValue(key, snow_shot::customAiModelsToJson({model})),
            "invalidate the selected model");
    require(retried && completions == 2 && invalidations == 0 &&
                job->state() == TranslationJob::State::Completed,
            "a retried invalidation emits only the replacement generation's completion");
    model.apiKey = QStringLiteral("another-key");
    require(settings.setValue(key, snow_shot::customAiModelsToJson({model})),
            "invalidate the model without retrying again");
    require(invalidations == 1 && completions == 2 &&
                job->state() == TranslationJob::State::Invalidated,
            "an invalidation that remains current still emits its terminal notification");
}

void customModelsStayAvailableAndInvalidateTogether(const QString& directory) {
    Server builtIn;
    builtIn.holdModels = true;
    Server custom;
    custom.streamPath = QByteArrayLiteral("/v1/chat/completions");
    snow_shot::CustomAiModelConfiguration model{
        QStringLiteral("11111111-1111-4111-8111-111111111111"),
        QStringLiteral("Local model"),
        custom.url() + QStringLiteral("/v1"),
        QStringLiteral("test-key"),
        QStringLiteral("provider-model"),
        true};
    ConfigurationStore settings(directory + QStringLiteral("/custom.json"), true, true, 60000);
    const QString key = QStringLiteral("api_configuration/custom_models");
    require(settings.setValue(key, snow_shot::customAiModelsToJson({model})),
            "save custom configuration");
    SnowShotApiClient client(builtIn.url());
    auto& service = TranslationService::forClient(client, settings, QLocale::English);
    require(&service == &TranslationService::forClient(client, settings, QLocale::English),
            "consumers share the catalog and settings owner");
    require(service.models().size() == 1 && service.models().first().supportsVision &&
                service.preferences().modelId == model.selectionId(),
            "custom vision model is immediately eligible");
    {
        QObject fixedOwner;
        auto preferences = service.preferences();
        preferences.sourceLanguage = QStringLiteral("en");
        preferences.targetLanguage = QStringLiteral("ja");
        auto* fixed = service.createJob({QStringLiteral(" ")}, preferences, &fixedOwner);
        const auto global = service.preferences();
        fixed->start();
        fixed->retry();
        require(
            fixed->preferences() == preferences && service.preferences() == global,
            "explicit per-job preferences survive start/retry without changing global settings");
    }
    service.refreshModels();
    service.refreshModels();
    QObject owner;
    auto* first = service.createJob({QStringLiteral("same input")}, &owner);
    auto* second = service.createJob({QStringLiteral("same input")}, &owner);
    first->start();
    // Identical parallel requests can reach the server in either order. Establish the
    // first socket's owner before starting the second, while retaining overlapping jobs.
    waitUntil([&] { return custom.streams.size() == 1 && builtIn.modelRequests == 1; },
              "first custom stream does not wait for builtin discovery");
    second->start();
    waitUntil([&] { return custom.streams.size() == 2 && builtIn.modelRequests == 1; },
              "custom streams do not wait for coalesced builtin discovery");
    require(custom.streams[0].body == custom.streams[1].body &&
                custom.streams[0].body.value(QStringLiteral("model")) == model.model &&
                custom.streams[0].headers.contains("Authorization: Bearer test-key") &&
                !custom.streams[0].headers.contains(model.selectionId().toUtf8()),
            "consumers produce identical provider requests with scoped custom credentials");
    custom.delta(0, QStringLiteral("first output"));
    custom.delta(1, QStringLiteral("second output"));
    waitUntil(
        [&] {
            return !first->units().first().text.isEmpty() &&
                   !second->units().first().text.isEmpty();
        },
        "both jobs receive text");
    model.name = QStringLiteral("Renamed model");
    model.supportsVision = false;
    require(settings.setValue(key, snow_shot::customAiModelsToJson({model})),
            "rename and change vision capability");
    require(first->busy() && second->busy() && service.models().first().name == model.name,
            "display metadata edits preserve text translation");
    custom.finish(0);
    waitUntil([&] { return !first->busy(); }, "cache one completed result");
    model.apiKey = QStringLiteral("updated-key");
    require(settings.setValue(key, snow_shot::customAiModelsToJson({model})),
            "edit connection settings");
    waitUntil([&] { return custom.disconnected(1); }, "connection edit aborts affected stream");
    require(first->state() == TranslationJob::State::Invalidated &&
                second->state() == TranslationJob::State::Invalidated &&
                first->units().first().text.isEmpty() && second->units().first().text.isEmpty() &&
                custom.streams.size() == 2,
            "completed and running results invalidate without automatic restart");
    second->retry();
    waitUntil([&] { return custom.streams.size() == 3; },
              "explicit retry starts updated connection");
    require(custom.streams.last().headers.contains("Authorization: Bearer updated-key"),
            "retry uses new credentials");
    builtIn.respondModels();
    waitUntil([&] { return !service.loadingModels(); }, "finish builtin discovery");
    require(settings.setValue(key, QJsonArray{}), "delete selected custom model");
    require(second->state() == TranslationJob::State::Invalidated &&
                service.preferences().modelId == QStringLiteral("general") &&
                custom.streams.size() == 3 && builtIn.streams.isEmpty(),
            "deletion selects shared fallback without sending another translation");
}

void failuresAndOwnerLifetimes(const QString& directory) {
    Server server;
    ConfigurationStore settings(directory + QStringLiteral("/lifecycle.json"), true, true, 60000);
    auto client = std::make_unique<SnowShotApiClient>(server.url());
    auto& service = TranslationService::forClient(*client, settings, QLocale::English);
    QObject owner;
    auto* empty = service.createJob({QStringLiteral("   ")}, &owner);
    empty->start();
    require(empty->state() == TranslationJob::State::Completed && server.modelRequests == 0,
            "empty source does not perform discovery");
    auto* job = service.createJob({QStringLiteral("hello")}, &owner);
    job->start();
    waitUntil([&] { return server.streams.size() == 1; }, "prepare nonempty input");
    server.finish(0);
    waitUntil([&] { return !job->busy(); }, "empty response finishes");
    require(job->state() == TranslationJob::State::Failed && !job->errorText().isEmpty(),
            "empty model response is an actionable error");
    job->retry();
    waitUntil([&] { return server.streams.size() == 2; }, "retry empty response");
    server.delta(1, QStringLiteral("partial"));
    waitUntil([&] { return !job->units().first().text.isEmpty(); }, "deliver partial response");
    server.fail(1);
    waitUntil([&] { return !job->busy(); }, "fail partial response");
    require(job->units().first().text == QStringLiteral("partial"),
            "ordinary errors retain partial output");
    job->retry();
    waitUntil([&] { return server.streams.size() == 3; }, "start before client destruction");
    client.reset();
    require(!job->busy() && !job->errorText().isEmpty(),
            "provider destruction settles the job safely");
}
void serverChangesRefreshModelsAndPreserveStreams(const QString& directory) {
    Server first, second, custom;
    second.holdModels = true;
    custom.streamPath = QByteArrayLiteral("/v1/chat/completions");
    ConfigurationStore settings(directory + QStringLiteral("/servers.json"), true, true, 60000);
    const QString serverKey = QStringLiteral("api_configuration/server_url");
    const snow_shot::CustomAiModelConfiguration customModel{
        QStringLiteral("11111111-1111-4111-8111-111111111111"),
        QStringLiteral("Custom"),
        custom.url() + QStringLiteral("/v1"),
        QStringLiteral("test-key"),
        QStringLiteral("provider-model"),
        true};
    require(settings.setValue(QStringLiteral("api_configuration/custom_models"),
                              snow_shot::customAiModelsToJson({customModel})),
            "save custom model");
    SnowShotApiClient client(first.url());
    auto& service = TranslationService::forClient(client, settings, QLocale::English);
    service.refreshModels();
    waitUntil([&] { return !service.loadingModels(); }, "initial server catalog loaded");
    auto preferences = service.preferences();
    preferences.modelId = QStringLiteral("vision");
    require(service.savePreferences(preferences), "select model that exists on both servers");
    QObject receiver;
    bool originalDone = false;
    SnowShotTranslationRequest input;
    input.model = QStringLiteral("general");
    input.text = QStringLiteral("hello");
    require(client.streamTranslation(
                input, &receiver, [](const QString&) {}, [&](auto) { originalDone = true; }) != 0,
            "start original server stream");
    waitUntil([&] { return first.streams.size() == 1; }, "original stream is active");
    require(settings.setValue(serverKey, second.url()), "save replacement server");
    waitUntil([&] { return second.modelRequests == 1; }, "new server catalog refresh starts");
    require(service.loadingModels() && service.preferences().modelId == QStringLiteral("vision") &&
                !client.hasBuiltInModels(QLocale(QLocale::English).name()),
            "old catalog is cleared without prematurely replacing selected model");
    first.delta(0, QStringLiteral("old output"));
    first.finish(0);
    waitUntil([&] { return originalDone; }, "old stream finishes after switch");
    second.respondModels();
    waitUntil([&] { return !service.loadingModels(); }, "replacement catalog loaded");
    require(service.preferences().modelId == QStringLiteral("vision"),
            "shared selected model survives server change");
    require(client.streamTranslation(
                input, &receiver, [](const QString&) {}, [](auto) {}) != 0,
            "new built-in stream starts");
    waitUntil([&] { return second.streams.size() == 1; }, "new stream reaches replacement server");
    input.model = customModel.selectionId();
    require(client.streamTranslation(
                input, &receiver, [](const QString&) {}, [](auto) {}) != 0,
            "custom stream starts");
    waitUntil([&] { return custom.streams.size() == 1; }, "custom provider retains own address");
    second.finish(0);
    custom.finish(0);

    // A second switch while discovery is pending must retire the first refresh.
    first.holdModels = true;
    require(settings.setValue(serverKey, first.url()), "start another held discovery");
    waitUntil([&] { return first.modelRequests == 2; }, "discovery is in flight");
    second.models =
        QJsonArray{QJsonObject{{QStringLiteral("model"), QStringLiteral("replacement")},
                               {QStringLiteral("name"), QStringLiteral("Replacement")}}};
    require(settings.setValue(serverKey, second.url()), "switch during discovery");
    waitUntil([&] { return second.modelRequests == 2; }, "latest discovery is in flight");
    second.respondModels();
    waitUntil([&] { return !service.loadingModels(); }, "latest discovery finishes");
    first.respondModels();
    flushEvents();
    require(service.preferences().modelId != QStringLiteral("vision") &&
                client.baseUrl() == second.url() &&
                client.cachedChatModels().first().id == QStringLiteral("replacement"),
            "missing selection falls back and retired refresh cannot overwrite latest catalog");
}

void preferenceAndCatalogPublicationPermitServiceDeletion(const QString& directory) {
    for (const bool removeFromPersistence : {false, true}) {
        SnowShotApiClient client(QStringLiteral("http://127.0.0.1:1"));
        ConfigurationStore settings(
            directory + QStringLiteral("/save-lifetime-%1.json").arg(removeFromPersistence), true,
            true, 60000);
        QPointer<TranslationService> service =
            &TranslationService::forClient(client, settings, QLocale::English);
        int notifications = 0;
        const auto remove = [&] {
            ++notifications;
            delete service.data();
        };
        if (removeFromPersistence) {
            QObject::connect(&settings, &ConfigurationStore::valueChanged, &settings,
                             [&](const QString&, const QJsonValue&) {
                                 if (service)
                                     remove();
                             });
        } else {
            QObject::connect(service, &TranslationService::preferencesChanged, &settings,
                             [&](TranslationService::ChangeReason) { remove(); });
        }
        auto preferences = service->preferences();
        preferences.targetLanguage = QStringLiteral("ja");
        require(!service->savePreferences(preferences),
                "retired preference publication reports cancellation");
        require(!service && notifications == 1,
                "preference persistence and observers may retire their service");
    }

    for (const bool removeFromPersistence : {false, true}) {
        SnowShotApiClient client(QStringLiteral("http://127.0.0.1:1"));
        ConfigurationStore settings(
            directory + QStringLiteral("/catalog-lifetime-%1.json").arg(removeFromPersistence),
            true, true, 60000);
        QPointer<TranslationService> service =
            &TranslationService::forClient(client, settings, QLocale::English);
        int notifications = 0;
        const auto remove = [&] {
            ++notifications;
            delete service.data();
        };
        if (removeFromPersistence) {
            QObject::connect(&settings, &ConfigurationStore::valueChanged, &settings,
                             [&](const QString& key, const QJsonValue&) {
                                 if (key == QStringLiteral("screenshot_translation/model") &&
                                     service)
                                     remove();
                             });
        } else {
            QObject::connect(service, &TranslationService::preferencesChanged, &settings,
                             [&](TranslationService::ChangeReason) { remove(); });
        }
        const snow_shot::CustomAiModelConfiguration model{
            QStringLiteral("33333333-3333-4333-8333-333333333333"),
            QStringLiteral("Lifetime model"),
            QStringLiteral("http://127.0.0.1:1/v1"),
            QStringLiteral("test-key"),
            QStringLiteral("local-model"),
            true};
        client.setCustomModels({model});
        require(!service && notifications == 1,
                "catalog selection persistence and observers may retire their service");
    }
}

void nestedPreferenceSaveSupersedesPublication(const QString& directory) {
    SnowShotApiClient client(QStringLiteral("http://127.0.0.1:1"));
    ConfigurationStore settings(directory + QStringLiteral("/nested-preferences.json"), true, true,
                                60000);
    auto& service = TranslationService::forClient(client, settings, QLocale::English);
    bool nested = false;
    int catalogNotifications = 0;
    QObject::connect(&service, &TranslationService::catalogChanged, &settings,
                     [&] { ++catalogNotifications; });
    QObject::connect(&service, &TranslationService::preferencesChanged, &settings,
                     [&](TranslationService::ChangeReason) {
                         if (nested)
                             return;
                         nested = true;
                         auto preferences = service.preferences();
                         preferences.targetLanguage = QStringLiteral("zh-Hans");
                         require(service.savePreferences(preferences),
                                 "nested preference update completes");
                     });
    auto preferences = service.preferences();
    preferences.targetLanguage = QStringLiteral("ja");
    require(!service.savePreferences(preferences),
            "superseded preference publication reports cancellation");
    require(nested && service.preferences().targetLanguage == QStringLiteral("zh-Hans") &&
                settings.value(QStringLiteral("screenshot_translation/target_language")) ==
                    QStringLiteral("zh-Hans") &&
                catalogNotifications == 1,
            "latest preference update owns the remaining catalog notification");
}

void catalogMetadataChangesPreservePreferenceWrites(const QString& directory) {
    for (const bool fromPersistence : {false, true}) {
        SnowShotApiClient client{QString()};
        ConfigurationStore settings(
            directory + QStringLiteral("/catalog-during-save-%1.json").arg(fromPersistence), true,
            true, 60000);
        auto& service = TranslationService::forClient(client, settings, QLocale::English);
        snow_shot::CustomAiModelConfiguration model{
            QStringLiteral("33333333-3333-4333-8333-333333333333"),
            QStringLiteral("Initial name"),
            QStringLiteral("http://127.0.0.1:1/v1"),
            QStringLiteral("test-key"),
            QStringLiteral("local-model"),
            true};
        client.setCustomModels({model});
        require(service.preferences().modelId == model.selectionId(),
                "catalog metadata fixture starts with a selected custom model");
        bool refreshed = false;
        const auto refresh = [&] {
            if (refreshed)
                return;
            refreshed = true;
            model.name = QStringLiteral("Updated name");
            client.setCustomModels({model});
        };
        if (fromPersistence) {
            QObject::connect(&settings, &ConfigurationStore::valueChanged, &settings,
                             [&](const QString& key, const QJsonValue&) {
                                 if (key ==
                                     QStringLiteral("screenshot_translation/target_language"))
                                     refresh();
                             });
        } else {
            QObject::connect(&service, &TranslationService::preferencesChanged, &settings,
                             [&](TranslationService::ChangeReason) { refresh(); });
        }
        int preferenceNotifications = 0;
        QObject::connect(&service, &TranslationService::preferencesChanged, &settings,
                         [&](TranslationService::ChangeReason) { ++preferenceNotifications; });
        auto preferences = service.preferences();
        preferences.targetLanguage = QStringLiteral("ja");
        require(service.savePreferences(preferences),
                "catalog-only updates do not retire a successful preference save");
        require(refreshed && service.preferences() == preferences &&
                    settings.value(QStringLiteral("screenshot_translation/target_language")) ==
                        QStringLiteral("ja") &&
                    service.models().first().name == model.name && preferenceNotifications == 1,
                "catalog refresh keeps persisted and published translation preferences aligned");
    }
}

void catalogSelectionWritesRespectNestedUpdates(const QString& directory) {
    for (const bool saveNestedPreferences : {false, true}) {
        SnowShotApiClient client{QString()};
        ConfigurationStore settings(
            directory +
                QStringLiteral("/catalog-selection-write-%1.json").arg(saveNestedPreferences),
            true, true, 60000);
        auto& service = TranslationService::forClient(client, settings, QLocale::English);
        snow_shot::CustomAiModelConfiguration model{
            QStringLiteral("33333333-3333-4333-8333-333333333333"),
            QStringLiteral("Initial name"),
            QStringLiteral("http://127.0.0.1:1/v1"),
            QStringLiteral("test-key"),
            QStringLiteral("local-model"),
            true};
        bool nested = false;
        int preferenceNotifications = 0;
        TranslationService::ChangeReason reason = TranslationService::ChangeReason::Catalog;
        QObject::connect(&service, &TranslationService::preferencesChanged, &settings,
                         [&](TranslationService::ChangeReason nextReason) {
                             ++preferenceNotifications;
                             reason = nextReason;
                         });
        QObject::connect(&settings, &ConfigurationStore::valueChanged, &settings,
                         [&](const QString& key, const QJsonValue&) {
                             if (nested || key != QStringLiteral("screenshot_translation/model"))
                                 return;
                             nested = true;
                             if (saveNestedPreferences) {
                                 auto preferences = service.preferences();
                                 preferences.modelId = model.selectionId();
                                 preferences.targetLanguage = QStringLiteral("ja");
                                 require(service.savePreferences(preferences),
                                         "nested user preferences supersede catalog selection");
                             } else {
                                 model.name = QStringLiteral("Updated name");
                                 client.setCustomModels({model});
                             }
                         });
        client.setCustomModels({model});
        require(nested && service.preferences().modelId == model.selectionId() &&
                    settings.value(QStringLiteral("screenshot_translation/model")) ==
                        model.selectionId() &&
                    preferenceNotifications == 1,
                "catalog selection uses the persisted model while its cache awaits publication");
        require(reason == (saveNestedPreferences ? TranslationService::ChangeReason::UserPreferences
                                                 : TranslationService::ChangeReason::Catalog) &&
                    service.preferences().targetLanguage ==
                        (saveNestedPreferences ? QStringLiteral("ja") : QStringLiteral("en")),
                "only a genuine nested preference write supersedes catalog selection publication");
    }
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "isolated translation service settings");
    preferenceAndCatalogPublicationPermitServiceDeletion(directory.path());
    nestedPreferenceSaveSupersedesPublication(directory.path());
    catalogMetadataChangesPreservePreferenceWrites(directory.path());
    catalogSelectionWritesRespectNestedUpdates(directory.path());
    terminalNotificationsRespectReentrantTransitions(directory.path());
    failedNotificationsRespectReentrantRetry(directory.path());
    invalidationNotificationsRespectReentrantRetry(directory.path());
    if (application.arguments().contains(QStringLiteral("--notification-lifetime-only")))
        return 0;
    discoverySharesRequestsAndCancellation(directory.path());
    secondaryTargetsPersistAndReachEveryUnit(directory.path());
    detectionIsDeferredUntilRoutingNeedsIt(directory.path());
    completeInputRoutingReachesEveryProviderAndRetries(directory.path());
    failedUnitRetryKeepsCompletedInputContext(directory.path());
    duplicateUnitsShareRequestsAndRetryByOriginalPosition(directory.path());
    duplicateDeltaObserversMayCancelOrDestroyTheJob(directory.path());
    customModelsStayAvailableAndInvalidateTogether(directory.path());
    failuresAndOwnerLifetimes(directory.path());
    serverChangesRefreshModelsAndPreserveStreams(directory.path());
    return 0;
}
