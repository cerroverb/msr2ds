// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Randovania (randovania.org) randomizes Samus Returns: it places the items, and its exporter
// (Open Samus Returns Rando) patches the player's dump into a game mod (msr/mods.h). The
// launcher's Randomizer tab drives its command line with no window of its own: a release the
// launcher downloads into <user>/randovania/, or a copy the player already has. Qt thread only.

#pragma once

#include <functional>
#include <vector>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;
class QTimer;

namespace MSR::Randovania {

/// The randovania program run: the player's own copy if they chose one, else the downloaded
/// one. Empty when there is neither.
QString Program();
/// The downloaded release's version (11.1.1), or empty.
QString DownloadedVersion();
/// The player's own copy, or empty when the downloaded one is used.
QString OwnProgram();
/// Uses the player's own copy of the randovania program; empty goes back to the downloaded one.
void SetOwnProgram(const QString& path);

struct Preset {
    QString name;
    QString file;        ///< The player's .rdvpreset; empty for one included with Randovania
    QString description; ///< Plain text
};
/// Randovania's Starter Preset, then the Samus Returns presets the player made in Randovania.
std::vector<Preset> Presets();
/// The preset last generated with (its file, or the name of an included one).
QString LastPreset();

/// The seed log (seed.rdvgame) of the enabled randomizer mod, empty when there is none.
QString EnabledSeedLog();

/// Opens Randovania's own window: the main one (presets, options), or a seed's details, spoiler
/// log included, when `rdvgame` is given. False if it could not start.
bool OpenWindow(const QString& rdvgame = {});

/// Downloads Randovania or generates a seed, in the background, one job at a time.
class Task final : public QObject {
    Q_OBJECT

public:
    enum class Job { None, Download, Generate };

    explicit Task(QObject* parent);
    ~Task() override;

    Job Running() const {
        return job;
    }

    /// Downloads the latest Randovania release for this system into <user>/randovania/, unless
    /// that version is already there, and uses it.
    void Download();

    /// Generates a seed from `preset`, or from `permalink` when one is given, then patches `rom`
    /// into a new game mod, "Randomizer - <seed name>".
    void Generate(const Preset& preset, const QString& permalink, const QString& rom);

    /// Stops the job and removes what it left half done.
    void Cancel();

signals:
    /// What the job is doing, as a line for the launcher.
    void Progress(const QString& text);
    /// The job ended. On success `error` is empty, and `name` is the version downloaded or the
    /// new mod's folder, `detail` the seed's name and hash ("already" when the download was the
    /// version in use).
    void Finished(const QString& error, const QString& name, const QString& detail);

private:
    using Next = std::function<void(const QByteArray& output)>;

    void Begin(Job kind);
    /// Runs one command; `next` continues with its output when it succeeds. With `watch`, the
    /// progress is that file's size against `total` bytes.
    void Run(const QString& step, const QString& program, const QStringList& arguments, Next next,
             const QString& watch = {}, qint64 total = 0);
    void Fail(const QString& error);
    void Succeed(const QString& name, const QString& detail);
    void End();
    void Tick();
    void Log(const QByteArray& text);

    Job job = Job::None;
    QProcess* process = nullptr;
    QTimer* timer = nullptr;
    QElapsedTimer clock;
    QString label;
    QString watch_file;
    qint64 watch_total = 0;
    QByteArray output;
    QString last_line;
    /// Removes the job's partial results when it fails or is cancelled.
    std::function<void()> cleanup;
};

} // namespace MSR::Randovania
