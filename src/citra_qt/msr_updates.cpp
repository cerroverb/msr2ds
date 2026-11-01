// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <utility>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QStringList>
#include "citra_qt/msr_process.h"
#include "citra_qt/msr_updates.h"
#include "common/logging/log.h"
#include "msr/game_folder.h"
#include "msr/version.h"

namespace MSR::Updates {
namespace {

const QString StartupKey = QStringLiteral("updates/check_at_startup");

QString SettingsPath() {
    return GameFolder::User() + QStringLiteral("msr/settings.ini");
}

/// "v1.1.0-pre": the numbers (1, 1, 0) and what follows the dash ("pre").
struct Parsed {
    QList<int> numbers;
    QString suffix;
};

Parsed Parse(QString version) {
    version = version.trimmed();
    if (version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        version.remove(0, 1);
    }
    Parsed parsed;
    const qsizetype dash = version.indexOf(QLatin1Char('-'));
    if (dash >= 0) {
        parsed.suffix = version.mid(dash + 1);
        version.truncate(dash);
    }
    for (const QString& part : version.split(QLatin1Char('.'))) {
        parsed.numbers << part.toInt();
    }
    return parsed;
}

QString WithoutV(const QString& tag) {
    return tag.startsWith(QLatin1Char('v'), Qt::CaseInsensitive) ? tag.mid(1) : tag;
}

} // namespace

bool CheckAtStartup() {
    return QSettings(SettingsPath(), QSettings::IniFormat).value(StartupKey, true).toBool();
}

void SetCheckAtStartup(bool check) {
    QSettings(SettingsPath(), QSettings::IniFormat).setValue(StartupKey, check);
}

QString Current() {
    const QString test = qEnvironmentVariable("MSR_UPDATE_TEST_VERSION");
    return test.isEmpty() ? QString::fromLatin1(MSR::Version) : test;
}

int Compare(const QString& a, const QString& b) {
    const Parsed left = Parse(a);
    const Parsed right = Parse(b);
    const qsizetype count = std::max(left.numbers.size(), right.numbers.size());
    for (qsizetype i = 0; i < count; ++i) {
        const int x = i < left.numbers.size() ? left.numbers[i] : 0;
        const int y = i < right.numbers.size() ? right.numbers[i] : 0;
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    if (left.suffix.isEmpty() != right.suffix.isEmpty()) {
        return left.suffix.isEmpty() ? 1 : -1;
    }
    return QString::compare(left.suffix, right.suffix, Qt::CaseInsensitive);
}

Checker::Checker(QObject* parent) : QObject(parent) {}

Checker::~Checker() {
    if (QProcess* running = std::exchange(process, nullptr)) {
        running->disconnect(this);
        running->kill();
        running->waitForFinished(2000);
        delete running;
    }
}

void Checker::Start() {
    if (process) {
        return;
    }
    const QString curl = Process::SystemTool(QStringLiteral("curl"));
    if (curl.isEmpty()) {
        emit Finished(QStringLiteral("curl, which the check uses, was not found."), {}, {}, {});
        return;
    }
    process = new QProcess(this);
    Process::HideConsole(*process);
    process->setProgram(curl);
    process->setArguments(
        {QStringLiteral("-sSfL"), QStringLiteral("--max-time"), QStringLiteral("20"),
         QStringLiteral("-H"), QStringLiteral("Accept: application/vnd.github+json"),
         QStringLiteral("https://api.github.com/repos/%1/releases?per_page=30")
             .arg(QLatin1String(MSR::ReleasesRepo))});
    connect(process, &QProcess::finished, this, [this](int code) { Done(code); });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            std::exchange(process, nullptr)->deleteLater();
            emit Finished(QStringLiteral("curl could not start."), {}, {}, {});
        }
    });
    process->start();
}

void Checker::Done(int code) {
    QProcess* done = std::exchange(process, nullptr);
    done->deleteLater();
    if (code != 0 || done->exitStatus() != QProcess::NormalExit) {
        // curl's own message, such as "curl: (6) Could not resolve host: api.github.com".
        QString error = QString::fromUtf8(done->readAllStandardError()).trimmed();
        error = error.section(QLatin1Char('\n'), -1).trimmed();
        if (error.isEmpty()) {
            error = QStringLiteral("GitHub could not be reached.");
        }
        LOG_WARNING(Frontend, "Update check: {}", error.toStdString());
        emit Finished(error, {}, {}, {});
        return;
    }
    const QJsonDocument reply = QJsonDocument::fromJson(done->readAllStandardOutput());
    if (!reply.isArray()) {
        emit Finished(QStringLiteral("GitHub's answer could not be read."), {}, {}, {});
        return;
    }
    const QString current = Current();
    const bool on_prerelease = current.contains(QLatin1Char('-'));
    QString newest, title, url;
    for (const auto& value : reply.array()) {
        const QJsonObject release = value.toObject();
        if (release.value(QStringLiteral("draft")).toBool() ||
            (release.value(QStringLiteral("prerelease")).toBool() && !on_prerelease)) {
            continue;
        }
        const QString tag = release.value(QStringLiteral("tag_name")).toString();
        if (tag.isEmpty() || Compare(tag, current) <= 0 ||
            (!newest.isEmpty() && Compare(tag, newest) <= 0)) {
            continue;
        }
        newest = tag;
        title = release.value(QStringLiteral("name")).toString();
        url = release.value(QStringLiteral("html_url")).toString();
    }
    LOG_INFO(Frontend, "Update check: {} is {}", current.toStdString(),
             newest.isEmpty() ? std::string("the latest") : "older than " + newest.toStdString());
    emit Finished({}, WithoutV(newest), title, url);
}

} // namespace MSR::Updates
