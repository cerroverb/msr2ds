// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <utility>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QVariant>
#include "citra_qt/msr_process.h"
#include "citra_qt/msr_randovania.h"
#include "common/logging/log.h"
#include "msr/game_folder.h"
#include "msr/mods.h"

namespace MSR::Randovania {
namespace {

/// Randovania's name for the game, and the preset it ships for new players.
constexpr char Game[] = "samus_returns";
const QString StarterPreset = QStringLiteral("Starter Preset");

constexpr char LatestRelease[] = "https://api.github.com/repos/randovania/randovania/releases/latest";
#ifdef _WIN32
constexpr char AssetSuffix[] = "-windows.zip";
constexpr char ArchiveName[] = "download.zip";
constexpr char ProgramName[] = "randovania.exe";
#else
constexpr char AssetSuffix[] = "-linux.tar.gz";
constexpr char ArchiveName[] = "download.tar.gz";
constexpr char ProgramName[] = "randovania";
#endif

/// <user>/randovania/: the downloaded release (one folder per version), the seed being made and
/// the last job's log.
QString Folder() {
    return GameFolder::User() + QStringLiteral("randovania/");
}

/// Kept with the mods' settings, in user/msr/settings.ini, under [randovania].
QVariant Setting(const char* key) {
    const QSettings file(GameFolder::User() + QStringLiteral("msr/settings.ini"),
                         QSettings::IniFormat);
    return file.value(QStringLiteral("randovania/") + QLatin1String(key));
}

/// Stores `value`, or removes the key when it is empty.
void SetSetting(const char* key, const QString& value) {
    QSettings file(GameFolder::User() + QStringLiteral("msr/settings.ini"), QSettings::IniFormat);
    const QString name = QStringLiteral("randovania/") + QLatin1String(key);
    if (value.isEmpty()) {
        file.remove(name);
    } else {
        file.setValue(name, value);
    }
}

QString Native(const QString& path) {
    return QDir::toNativeSeparators(path);
}

/// The randovania program in an unpacked release: the one nearest its top folder.
QString FindProgram(const QString& folder) {
    QString best;
    QDirIterator it(folder, {QLatin1String(ProgramName)}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (QFileInfo(path).isExecutable() &&
            (best.isEmpty() || path.count(QLatin1Char('/')) < best.count(QLatin1Char('/')))) {
            best = path;
        }
    }
    return best;
}

QString DownloadedProgram() {
    const QString relative = Setting("downloaded_program").toString();
    if (relative.isEmpty()) {
        return {};
    }
    const QString path = Folder() + relative;
    return QFileInfo(path).isFile() ? path : QString{};
}

/// Randovania's own Qt and Python must not pick up the launcher's environment (the AppImage
/// sets plugin and library paths).
QProcessEnvironment Environment() {
    auto env = QProcessEnvironment::systemEnvironment();
    for (const char* name : {"QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML2_IMPORT_PATH",
                             "PYTHONHOME", "PYTHONPATH", "LD_LIBRARY_PATH"}) {
        env.remove(QLatin1String(name));
    }
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    return env;
}

/// No console window for Randovania (a console program on Windows), curl or tar.
void Prepare(QProcess& process) {
    process.setProcessEnvironment(Environment());
    process.setWorkingDirectory(Folder());
    Process::HideConsole(process);
}

/// A preset's description, as plain text of at most two lines in the launcher.
QString PlainText(const QString& html) {
    QString text = QTextDocumentFragment::fromHtml(html).toPlainText().simplified();
    if (text.size() > 160) {
        text = text.left(157).trimmed() + QStringLiteral("...");
    }
    return text;
}

/// A seed's name as a folder name.
QString FolderSafe(QString name) {
    name.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])")), QStringLiteral(" "));
    return name.simplified();
}

/// The last line a command printed, short: no terminal colours, no "[ 45%]" (the exporter's
/// count, which runs past 100), and only the name of a file it writes ("Writing D:\...\x.pkg").
QString LastLine(const QByteArray& output) {
    static const QRegularExpression ansi(QStringLiteral("\x1b\\[[0-9;]*[A-Za-z]"));
    static const QRegularExpression percent(QStringLiteral(R"(^\[\s*\d+%\]\s*)"));
    const QStringList lines =
        QString::fromUtf8(output).split(QRegularExpression(QStringLiteral("[\r\n]")),
                                        Qt::SkipEmptyParts);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        QString line = it->trimmed();
        line.remove(ansi);
        line.remove(percent);
        if (line.startsWith(QStringLiteral("[PYI-"))) {
            continue; // the frozen program's own "Failed to execute script"
        }
        if (const qsizetype slash = std::max(line.lastIndexOf(QLatin1Char('/')),
                                             line.lastIndexOf(QLatin1Char('\\')));
            slash >= 0) {
            const qsizetype space = line.indexOf(QLatin1Char(' '));
            line = (space > 0 && space < slash ? line.left(space + 1) : QString{}) +
                   line.mid(slash + 1);
        }
        if (!line.isEmpty()) {
            return line;
        }
    }
    return {};
}

/// Why a command failed: the message of the Python exception it ended with, else its last line.
QString Reason(const QByteArray& output) {
    static const QRegularExpression exception(
        QStringLiteral(R"(^(?:[A-Za-z_]\w*\.)*[A-Za-z_]\w*(?:Error|Exception): (.+)$)"));
    const QStringList lines =
        QString::fromUtf8(output).split(QRegularExpression(QStringLiteral("[\r\n]")),
                                        Qt::SkipEmptyParts);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (const auto match = exception.match(it->trimmed()); match.hasMatch()) {
            return match.captured(1).trimmed();
        }
    }
    return LastLine(output);
}

} // namespace

QString Program() {
    if (const QString own = OwnProgram(); !own.isEmpty()) {
        return own;
    }
    return DownloadedProgram();
}

QString DownloadedVersion() {
    return DownloadedProgram().isEmpty()
               ? QString{}
               : Setting("downloaded_version").toString();
}

QString OwnProgram() {
    const QString path = Setting("own_program").toString();
    return !path.isEmpty() && QFileInfo(path).isFile() ? path : QString{};
}

void SetOwnProgram(const QString& path) {
    SetSetting("own_program", QDir::fromNativeSeparators(path));
}

std::vector<Preset> Presets() {
    std::vector<Preset> presets{
        {StarterPreset, {},
         QStringLiteral("For new players: no tricks or glitches needed, Scan Pulse from the "
                        "start, amiibo Reserve Tanks among the items.")}};
    // Presets the player made in Randovania (its user data folder).
#ifdef _WIN32
    const QString base = QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA"));
#else
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/.config");
    }
#endif
    if (base.isEmpty()) {
        return presets;
    }
    const QDir folder(base + QStringLiteral("/Randovania/presets"));
    std::vector<Preset> own;
    for (const QFileInfo& info :
         folder.entryInfoList({QStringLiteral("*.rdvpreset")}, QDir::Files)) {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonObject preset = QJsonDocument::fromJson(file.readAll()).object();
        if (preset.value(QStringLiteral("game")).toString() != QLatin1String(Game)) {
            continue;
        }
        const QString name = preset.value(QStringLiteral("name")).toString();
        own.push_back({name.isEmpty() ? info.completeBaseName() : name, info.absoluteFilePath(),
                       PlainText(preset.value(QStringLiteral("description")).toString())});
    }
    std::sort(own.begin(), own.end(), [](const Preset& a, const Preset& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    presets.insert(presets.end(), own.begin(), own.end());
    return presets;
}

QString LastPreset() {
    return Setting("preset").toString();
}

QString EnabledSeedLog() {
    for (const auto& mod : Mods::GameMods()) {
        if (mod.enabled && Mods::IsRandomizer(mod)) {
            const QString log = mod.path + QStringLiteral("seed.rdvgame");
            return QFile::exists(log) ? log : QString{};
        }
    }
    return {};
}

bool OpenWindow(const QString& rdvgame) {
    const QString program = Program();
    if (program.isEmpty()) {
        return false;
    }
    QProcess process;
    Prepare(process);
    process.setProgram(program);
    process.setArguments(rdvgame.isEmpty()
                             ? QStringList{QStringLiteral("gui"), QStringLiteral("main")}
                             : QStringList{QStringLiteral("gui"), QStringLiteral("game"),
                                           Native(rdvgame)});
    LOG_INFO(Frontend, "Randovania: {} {}", program.toStdString(),
             process.arguments().join(QLatin1Char(' ')).toStdString());
    return process.startDetached();
}

Task::Task(QObject* parent) : QObject(parent) {
    timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, &Task::Tick);
}

Task::~Task() {
    // The launcher is going away: stop without telling it.
    blockSignals(true);
    Cancel();
}

void Task::Begin(Job kind) {
    job = kind;
    cleanup = nullptr;
    QDir().mkpath(Folder());
    QFile log(Folder() + QStringLiteral("last-run.log"));
    if (log.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        log.write("Samus Returns launcher, Randovania job\n");
    }
}

void Task::Log(const QByteArray& text) {
    QFile log(Folder() + QStringLiteral("last-run.log"));
    if (log.open(QIODevice::WriteOnly | QIODevice::Append)) {
        log.write(text);
    }
}

void Task::Run(const QString& step, const QString& program, const QStringList& arguments, Next next,
               const QString& watch, qint64 total) {
    label = step;
    watch_file = watch;
    watch_total = total;
    output.clear();
    last_line.clear();
    clock.start();
    const QString command = program + QLatin1Char(' ') + arguments.join(QLatin1Char(' '));
    LOG_INFO(Frontend, "Randovania: {}", command.toStdString());
    Log("\n> " + command.toUtf8() + "\n");

    process = new QProcess(this);
    Prepare(*process);
    process->setProgram(program);
    process->setArguments(arguments);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::readyRead, this, [this] {
        const QByteArray chunk = process->readAll();
        output += chunk;
        Log(chunk);
        if (const QString line = LastLine(chunk); !line.isEmpty()) {
            last_line = line;
        }
    });
    connect(process, &QProcess::errorOccurred, this, [this, program](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) {
            return;
        }
        std::exchange(process, nullptr)->deleteLater();
        Fail(QStringLiteral("%1: %2 could not start.").arg(label, Native(program)));
    });
    connect(process, &QProcess::finished, this,
            [this, next = std::move(next)](int code, QProcess::ExitStatus status) {
                QProcess* done = std::exchange(process, nullptr);
                output += done->readAll();
                done->deleteLater();
                Log(QStringLiteral("[exit %1]\n").arg(code).toUtf8());
                if (status != QProcess::NormalExit || code != 0) {
                    const QString line = Reason(output);
                    Fail(line.isEmpty() ? QStringLiteral("%1 failed.").arg(label)
                                        : QStringLiteral("%1 failed: %2").arg(label, line));
                    return;
                }
                next(output);
            });
    process->start();
    timer->start();
    Tick();
}

void Task::Tick() {
    if (job == Job::None) {
        return;
    }
    QString text = label;
    if (watch_total > 0) {
        const qint64 size = QFileInfo(watch_file).size();
        text += QStringLiteral(": %1 of %2 MB")
                    .arg(size / (1024 * 1024))
                    .arg(watch_total / (1024 * 1024));
    } else {
        text += QStringLiteral(" (%1 s)").arg(clock.elapsed() / 1000);
        if (!last_line.isEmpty()) {
            text += QStringLiteral(": ") + last_line.left(90);
        }
    }
    emit Progress(text);
}

void Task::Fail(const QString& error) {
    if (cleanup) {
        std::exchange(cleanup, nullptr)();
    }
    LOG_WARNING(Frontend, "Randovania: {}", error.toStdString());
    Log("\n" + error.toUtf8() + "\n");
    End();
    emit Finished(error, {}, {});
}

void Task::Succeed(const QString& name, const QString& detail) {
    cleanup = nullptr;
    End();
    emit Finished({}, name, detail);
}

void Task::End() {
    timer->stop();
    job = Job::None;
}

void Task::Cancel() {
    if (job == Job::None) {
        return;
    }
    if (QProcess* running = std::exchange(process, nullptr)) {
        running->disconnect(this);
        running->kill();
        running->waitForFinished(5000);
        delete running;
    }
    Fail(QStringLiteral("Stopped."));
}

void Task::Download() {
    Begin(Job::Download);
    const QString curl = Process::SystemTool(QStringLiteral("curl"));
    const QString tar = Process::SystemTool(QStringLiteral("tar"));
    if (curl.isEmpty() || tar.isEmpty()) {
        Fail(QStringLiteral("Downloading needs curl and tar, which this system does not have."));
        return;
    }
    const QString root = Folder();
    Run(QStringLiteral("Looking for the latest Randovania"), curl,
        {QStringLiteral("-sSfL"), QStringLiteral("--max-time"), QStringLiteral("60"),
         QStringLiteral("-H"), QStringLiteral("Accept: application/vnd.github+json"),
         QLatin1String(LatestRelease)},
        [this, curl, tar, root](const QByteArray& reply) {
            const QJsonObject release = QJsonDocument::fromJson(reply).object();
            QString version = release.value(QStringLiteral("tag_name")).toString();
            if (version.startsWith(QLatin1Char('v'))) {
                version.remove(0, 1);
            }
            QString url;
            qint64 size = 0;
            for (const auto& value : release.value(QStringLiteral("assets")).toArray()) {
                const QJsonObject asset = value.toObject();
                if (asset.value(QStringLiteral("name")).toString().endsWith(QLatin1String(AssetSuffix))) {
                    url = asset.value(QStringLiteral("browser_download_url")).toString();
                    size = asset.value(QStringLiteral("size")).toInteger();
                }
            }
            static const QRegularExpression version_format(QStringLiteral(R"(^\d+(\.\d+)+$)"));
            if (url.isEmpty() || !version_format.match(version).hasMatch()) {
                Fail(QStringLiteral("No Randovania download for this system was found."));
                return;
            }
            if (version == DownloadedVersion()) {
                SetOwnProgram({});
                Succeed(version, QStringLiteral("already"));
                return;
            }
            const QString archive = root + QLatin1String(ArchiveName);
            const QString unpack = root + version + QStringLiteral(".part/");
            cleanup = [archive, unpack] {
                QFile::remove(archive);
                QDir(unpack).removeRecursively();
            };
            QFile::remove(archive);
            Run(QStringLiteral("Downloading Randovania %1").arg(version), curl,
                {QStringLiteral("-sSfL"), QStringLiteral("--retry"), QStringLiteral("2"),
                 QStringLiteral("-o"), Native(archive), url},
                [this, tar, root, version, archive, unpack](const QByteArray&) {
                    QDir(unpack).removeRecursively();
                    QDir().mkpath(unpack);
                    Run(QStringLiteral("Unpacking Randovania %1").arg(version), tar,
                        {QStringLiteral("-xf"), Native(archive), QStringLiteral("-C"), Native(unpack)},
                        [this, root, version, archive, unpack](const QByteArray&) {
                            QFile::remove(archive);
                            const QString found = FindProgram(unpack);
                            if (found.isEmpty()) {
                                Fail(QStringLiteral("The download has no %1 in it.")
                                         .arg(QLatin1String(ProgramName)));
                                return;
                            }
                            const QString relative = QDir(unpack).relativeFilePath(found);
                            const QString target = root + version;
                            QDir(target).removeRecursively();
                            if (!QDir().rename(QDir::cleanPath(unpack), target)) {
                                Fail(QStringLiteral("Could not move Randovania into %1.")
                                         .arg(Native(target)));
                                return;
                            }
                            cleanup = nullptr;
                            // Older downloads go.
                            static const QRegularExpression version_folder(
                                QStringLiteral(R"(^\d+(\.\d+)+$)"));
                            for (const QString& name :
                                 QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                                if (name != version && version_folder.match(name).hasMatch()) {
                                    QDir(root + name).removeRecursively();
                                }
                            }
                            SetSetting("downloaded_version", version);
                            SetSetting("downloaded_program", version + QLatin1Char('/') + relative);
                            SetSetting("own_program", {});
                            Succeed(version, {});
                        });
                },
                archive, size);
        });
}

void Task::Generate(const Preset& preset, const QString& permalink, const QString& rom) {
    Begin(Job::Generate);
    const QString program = Program();
    if (program.isEmpty()) {
        Fail(QStringLiteral("Set up Randovania first."));
        return;
    }
    const QString work = Folder() + QStringLiteral("seed/");
    QDir(work).removeRecursively();
    QDir().mkpath(work);
    cleanup = [work] { QDir(work).removeRecursively(); };
    const QString rdvgame = work + QStringLiteral("seed.rdvgame");

    QStringList arguments{QStringLiteral("layout")};
    if (!permalink.isEmpty()) {
        arguments << QStringLiteral("generate-from-permalink") << QStringLiteral("--permalink")
                  << permalink;
    } else {
        // Randovania's own range for seed numbers.
        arguments << QStringLiteral("generate-from-presets") << QStringLiteral("--seed-number")
                  << QString::number(QRandomGenerator::global()->bounded(1u << 31));
        if (preset.file.isEmpty()) {
            arguments << QStringLiteral("--name") << QStringLiteral("samus_returns/") + preset.name;
        } else {
            arguments << QStringLiteral("--file") << Native(preset.file);
        }
        SetSetting("preset", preset.file.isEmpty() ? preset.name : preset.file);
    }
    arguments << QStringLiteral("--status-update") << Native(rdvgame);

    Run(QStringLiteral("Generating the seed"), program, arguments,
        [this, program, work, rdvgame, rom](const QByteArray&) {
            QFile file(rdvgame);
            if (!file.open(QIODevice::ReadOnly)) {
                Fail(QStringLiteral("Randovania made no seed."));
                return;
            }
            const QJsonObject info =
                QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("info")).toObject();
            file.close();
            // A multiworld permalink: this game is the first world.
            const QJsonArray presets = info.value(QStringLiteral("presets")).toArray();
            if (presets.isEmpty() ||
                presets.first().toObject().value(QStringLiteral("game")).toString() !=
                    QLatin1String(Game)) {
                Fail(QStringLiteral("That permalink is for another game."));
                return;
            }
            const QString words = info.value(QStringLiteral("word_hash")).toString();
            const QString hash = info.value(QStringLiteral("hash")).toString();
            const QString name =
                QStringLiteral("Randomizer - ") + FolderSafe(words.isEmpty() ? hash : words);
            const QString mod = Mods::GameModFolder() + name + QLatin1Char('/');
            QDir(mod).removeRecursively();
            cleanup = [work, mod] {
                QDir(work).removeRecursively();
                QDir(mod).removeRecursively();
            };
            const QString patcher = work + QStringLiteral("patcher.json");
            Run(QStringLiteral("Preparing the patch"), program,
                {QStringLiteral("layout"), QStringLiteral("patcher-data"),
                 QStringLiteral("--layout-file"), Native(rdvgame), QStringLiteral("--output"),
                 Native(patcher)},
                [this, program, work, rdvgame, rom, name, mod, patcher, words, hash](const QByteArray&) {
                    Run(QStringLiteral("Patching your game"), program,
                        {QStringLiteral("layout"), QStringLiteral("export-game"),
                         QStringLiteral("--patcher-data"), Native(patcher), QLatin1String(Game),
                         QStringLiteral("--input-file"), Native(rom),
                         QStringLiteral("--output-path"), Native(QDir::cleanPath(mod)),
                         QStringLiteral("--target-platform"), QStringLiteral("azahar")},
                        [this, work, rdvgame, name, mod, words, hash](const QByteArray&) {
                            const auto mods = Mods::GameMods();
                            const auto made = std::find_if(mods.begin(), mods.end(),
                                                           [&](const Mods::GameMod& entry) {
                                                               return entry.name == name;
                                                           });
                            if (made == mods.end() || !Mods::IsRandomizer(*made)) {
                                Fail(QStringLiteral("Randovania's export holds no randomizer."));
                                return;
                            }
                            // Kept for Randovania to show: spoiler log, tracker.
                            QFile::copy(rdvgame, made->path + QStringLiteral("seed.rdvgame"));
                            QDir(work).removeRecursively();
                            Succeed(name, hash.isEmpty() ? words
                                                         : QStringLiteral("%1 (%2)").arg(words, hash));
                        });
                });
        });
}

} // namespace MSR::Randovania
