// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The launcher's check for new versions: the releases of MSR::ReleasesRepo on GitHub, read with
// curl when the launcher opens (unless the player turned that off) or when asked. It only tells:
// the player downloads a new version from its release page. A build that is a pre-release hears
// about newer pre-releases too; a full release only about full releases. Qt thread only.

#pragma once

#include <QObject>
#include <QString>

class QProcess;

namespace MSR::Updates {

/// Whether the launcher checks when it opens (user/msr/settings.ini, [updates]; on at first).
bool CheckAtStartup();
void SetCheckAtStartup(bool check);

/// This build's version (MSR::Version), or MSR_UPDATE_TEST_VERSION for tests.
QString Current();

/// Orders versions such as v1.0, 1.1.0-pre and 1.1.0: negative when `a` is older than `b`. A
/// release is newer than its own pre-releases.
int Compare(const QString& a, const QString& b);

class Checker final : public QObject {
    Q_OBJECT

public:
    explicit Checker(QObject* parent);
    ~Checker() override;

    /// Starts a check, unless one is running.
    void Start();
    bool Running() const {
        return process != nullptr;
    }

signals:
    /// The check ended. `error` says why GitHub could not be read; otherwise `version` is the
    /// newest release newer than this build, with its title and page (empty: this is the latest).
    void Finished(const QString& error, const QString& version, const QString& title,
                  const QString& url);

private:
    void Done(int code);

    QProcess* process = nullptr;
};

} // namespace MSR::Updates
