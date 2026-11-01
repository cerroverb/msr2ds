// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
#include <tuple>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include "citra_qt/msr_controls.h"
#include "citra_qt/msr_launcher.h"
#include "citra_qt/msr_randovania.h"
#include "citra_qt/msr_updates.h"
#include "citra_qt/uisettings.h"
#include "citra_qt/util/util.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/loader/loader.h"
#include "core/loader/smdh.h"
#include "input_common/main.h"
#include "msr/frame_rate.h"
#include "msr/game_folder.h"
#include "msr/mods.h"
#include "msr/overlay.h"
#include "msr/saves.h"
#include "msr/version.h"

namespace MSR {
namespace {

/// The languages the North American game has text for, by 3DS system language (0x11F844):
/// us_english, us_french and us_spanish, and the European eu_german, eu_italian and eu_dutch.
/// The game keeps the system language only where the console's region has it (0x1007D6), so
/// those three need a European console; Japanese has a text file too, but not its font.
struct Language {
    const char* name;
    Service::CFG::SystemLanguage value;
    bool european;
};
constexpr std::array<Language, 6> Languages{{
    {"English", Service::CFG::LANGUAGE_EN, false},
    {"Français", Service::CFG::LANGUAGE_FR, false},
    {"Español", Service::CFG::LANGUAGE_ES, false},
    {"Deutsch", Service::CFG::LANGUAGE_DE, true},
    {"Italiano", Service::CFG::LANGUAGE_IT, true},
    {"Nederlands", Service::CFG::LANGUAGE_NL, true},
}};
constexpr s32 RegionEurope = 2;

/// The console region a language needs: Europe, or the game's own (North America).
void SetRegionFor(const Language& language) {
    Settings::values.region_value =
        language.european ? RegionEurope : Settings::REGION_VALUE_AUTO_SELECT;
}

// SDL_GameControllerButton order, as in msr/input_bridge.cpp. Both lower face buttons choose:
// the right one is the game's A (Xbox B), the bottom one a PC pad's usual confirm (Xbox A), and
// the launcher has no back action for either to clash with.
constexpr int ButtonA = 0;
constexpr int ButtonB = 1;
constexpr std::array<int, 2> Choose = {ButtonA, ButtonB};
constexpr int ButtonStart = 6;
constexpr int ButtonLeftShoulder = 9;
constexpr int ButtonRightShoulder = 10;
constexpr int DpadUp = 11;
constexpr int DpadDown = 12;
constexpr int DpadLeft = 13;
constexpr int DpadRight = 14;

/// The in-game menu's dark navy and cyan (msr/overlay.cpp), in Qt style sheet form.
const char* const StyleSheet = R"(
QWidget { outline: none; }
#MsrLauncher { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #0b1b2a, stop:1 #03080d); }
#MsrLauncher QLabel, #MsrLauncher QCheckBox { color: #dcf0ff; font-size: 15px; }
#card { background: rgba(8, 18, 28, 245); border: 1px solid rgba(61, 168, 219, 150); border-radius: 8px; }
#MsrLauncher QLabel#title { font-size: 28px; font-weight: 600; }
#MsrLauncher QLabel#dim { color: #7f9bb0; font-size: 14px; }
#MsrLauncher QLabel#section { color: #3da8db; font-size: 13px; font-weight: 600; }
QLineEdit { background: #0d1f2f; color: #dcf0ff; border: 1px solid #1f577a; border-radius: 4px;
            padding: 6px 8px; font-size: 14px; }
QPushButton { background: #14334d; color: #dcf0ff; border: 1px solid #1f577a; border-radius: 5px;
              padding: 7px 18px; font-size: 15px; }
QPushButton:hover { background: #296b8f; }
QPushButton:pressed { background: #3da8db; }
QPushButton:disabled { background: #0e2131; color: #536879; border-color: #16364d; }
#play { background: #1d6a96; font-size: 18px; font-weight: 600; padding: 9px 44px; }
#play:hover { background: #3da8db; }
#play:disabled { background: #0e2131; color: #536879; }
QCheckBox::indicator { width: 20px; height: 20px; border: 1px solid #296b8f; border-radius: 4px;
                       background: #142b40; }
QCheckBox::indicator:checked { background: #3da8db; border-color: #9fe4ff; }
QComboBox { background: #142b40; color: #dcf0ff; border: 1px solid #1f577a; border-radius: 4px;
            padding: 5px 10px; font-size: 15px; }
QComboBox QAbstractItemView { background: #0b1a28; color: #dcf0ff; selection-background-color: #296b8f; }
QComboBox:disabled, QLineEdit:disabled { background: #0e2131; color: #536879; border-color: #16364d; }
QSlider::groove:horizontal { height: 6px; background: #142b40; border-radius: 3px; }
QSlider::sub-page:horizontal { background: #296b8f; border-radius: 3px; }
QSlider::handle:horizontal { background: #3da8db; width: 16px; margin: -6px 0; border-radius: 8px; }
QTabWidget::pane { border: 1px solid #1f577a; border-radius: 6px; background: #091623; top: -1px; }
QTabBar::tab { background: #0f2638; color: #b9d6ea; padding: 8px 24px; border: 1px solid #1f577a;
               border-bottom: none; border-top-left-radius: 5px; border-top-right-radius: 5px;
               margin-right: 3px; font-size: 15px; }
QTabBar::tab:selected { background: #1f577a; color: #ffffff; }
QTabBar::tab:hover { background: #296b8f; }
QScrollArea { background: transparent; border: none; }
#bindlist { background: transparent; }
QScrollBar:vertical { background: #0b1a28; width: 10px; border-radius: 5px; }
QScrollBar::handle:vertical { background: #296b8f; border-radius: 5px; min-height: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
)";

/// The highlight around the focused option: the gamepad's cursor, like the in-game menu's.
class FocusRing final : public QWidget {
public:
    explicit FocusRing(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        hide();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(159, 228, 255), 2.0));
        painter.drawRoundedRect(QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5), 6.0, 6.0);
    }
};

QLabel* MakeLabel(const QString& text, const char* style = nullptr) {
    auto* label = new QLabel(text);
    if (style) {
        label->setObjectName(QString::fromLatin1(style));
    }
    return label;
}

QString Decimal(int value) {
    return QString::number(value / 100.0, 'f', 2);
}

QString Percent(int value) {
    return QStringLiteral("%1%").arg(value);
}

QGridLayout* MakeGrid(QWidget* page) {
    auto* grid = new QGridLayout(page);
    grid->setContentsMargins(22, 18, 22, 18);
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(12);
    grid->setColumnMinimumWidth(0, 230);
    grid->setColumnStretch(1, 1);
    return grid;
}

/// A scrolling list for mod rows (Launcher::FillModList), its rows in `list`.
QScrollArea* MakeModList(QWidget*& list) {
    list = new QWidget;
    list->setObjectName(QStringLiteral("bindlist"));
    auto* rows = new QGridLayout(list);
    rows->setContentsMargins(0, 0, 8, 0);
    rows->setHorizontalSpacing(12);
    rows->setVerticalSpacing(8);
    rows->setColumnStretch(1, 1);
    auto* area = new QScrollArea;
    area->setWidget(list);
    area->setWidgetResizable(true);
    area->setFocusPolicy(Qt::NoFocus);
    area->viewport()->setAutoFillBackground(false);
    return area;
}

} // namespace

Launcher::Launcher(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("MsrLauncher"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QString::fromUtf8(StyleSheet));

    auto* card = new QWidget;
    card->setObjectName(QStringLiteral("card"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    card->setMaximumWidth(940);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* column = new QVBoxLayout(card);
    column->setContentsMargins(30, 26, 30, 24);
    column->setSpacing(14);

    // Title, with the game's own icon once a dump is chosen.
    auto* header = new QHBoxLayout;
    header->setSpacing(16);
    icon = new QLabel;
    icon->setFixedSize(72, 72);
    icon->hide();
    header->addWidget(icon);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(MakeLabel(QStringLiteral("Metroid: Samus Returns"), "title"));
    titles->addWidget(MakeLabel(
        QStringLiteral("Single-screen PC edition, %1").arg(QLatin1String(MSR::VersionLabel)),
        "dim"));
    header->addLayout(titles, 1);
    // A newer version, when the update check finds one (msr_updates.h).
    update_banner = new QWidget;
    auto* banner = new QVBoxLayout(update_banner);
    banner->setContentsMargins(0, 0, 0, 0);
    banner->setSpacing(6);
    update_text = MakeLabel({});
    update_text->setStyleSheet(QStringLiteral("color: #86e39a;"));
    update_get = new QPushButton(QStringLiteral("Download page"));
    banner->addWidget(update_text, 0, Qt::AlignRight);
    banner->addWidget(update_get, 0, Qt::AlignRight);
    update_banner->hide();
    header->addWidget(update_banner, 0, Qt::AlignVCenter);
    connect(update_get, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl(update_url)); });
    column->addLayout(header);

    // The game dump: the package does not include the game.
    column->addWidget(MakeLabel(QStringLiteral("GAME"), "section"));
    auto* rom_row = new QHBoxLayout;
    rom_path = new QLineEdit;
    rom_path->setReadOnly(true);
    rom_path->setFocusPolicy(Qt::NoFocus);
    rom_path->setPlaceholderText(QStringLiteral("No game dump selected"));
    rom_row->addWidget(rom_path, 1);
    browse = new QPushButton(QStringLiteral("Browse..."));
    rom_row->addWidget(browse);
    column->addLayout(rom_row);
    rom_status = new QLabel;
    rom_status->setWordWrap(true);
    column->addWidget(rom_status);
    connect(browse, &QPushButton::clicked, this, &Launcher::Browse);

    // The same settings as the in-game menu.
    column->addWidget(MakeLabel(QStringLiteral("SETTINGS"), "section"));
    tabs = new QTabWidget;
    tabs->addTab(BuildGameTab(), QStringLiteral("Game"));
    tabs->addTab(BuildDisplayTab(), QStringLiteral("Display"));
    tabs->addTab(BuildHudTab(), QStringLiteral("HUD"));
    tabs->addTab(BuildControlsTab(), QStringLiteral("Controls"));
    tabs->addTab(BuildModsTab(), QStringLiteral("Mods"));
    tabs->addTab(BuildRandomizerTab(), QStringLiteral("Randomizer"));
    tabs->addTab(BuildTexturesTab(), QStringLiteral("Textures"));
    tabs->addTab(BuildCheatsTab(), QStringLiteral("Cheats"));
    tabs->setMinimumHeight(400);
    column->addWidget(tabs, 1);

    auto* footer = new QHBoxLayout;
    footer->setSpacing(12);
    auto* help = MakeLabel(QStringLiteral("Gamepad: A or B selects, Start plays, LB / RB switch "
                                          "tabs. In game, F1 or Select opens these settings."),
                           "dim");
    help->setWordWrap(true);
    footer->addWidget(help, 1);
    quit = new QPushButton(QStringLiteral("Quit"));
    play = new QPushButton(QStringLiteral("Play"));
    play->setObjectName(QStringLiteral("play"));
    footer->addWidget(quit);
    footer->addWidget(play);
    column->addLayout(footer);
    connect(quit, &QPushButton::clicked, this, &Launcher::QuitRequested);
    connect(play, &QPushButton::clicked, this, &Launcher::PlayRequested);

    // The card is centered, up to 940 pixels wide.
    auto* row = new QHBoxLayout;
    row->addStretch(1);
    row->addWidget(card, 10);
    row->addStretch(1);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    root->addStretch(1);
    root->addLayout(row, 10);
    root->addStretch(1);

    UpdateFocusOrder();
    tabs->setFocusPolicy(Qt::NoFocus);
    tabs->tabBar()->setFocusPolicy(Qt::StrongFocus);

    focus_ring = new FocusRing(this);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (now && bindings->isAncestorOf(now)) {
            bindings->ensureWidgetVisible(now, 0, 12);
        }
        for (QScrollArea* list : {mods_area, packs}) {
            if (now && list->isAncestorOf(now)) {
                list->ensureWidgetVisible(now, 0, 12);
            }
        }
        FollowFocus();
    });
    connect(tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (tabs->widget(index) == rando_page) {
            RefreshRandomizer();
        }
        FollowFocus();
    });

    // Gamepad: any connected controller, as the in-game menu reads it.
    for (int i = 0; i < ButtonCount; ++i) {
        Common::ParamPackage params({{"engine", "sdl"}, {"api", "controller"}, {"maptype", "all"}});
        params.Set("button", i);
        buttons[i] = Input::CreateDevice<Input::ButtonDevice>(params.Serialize());
    }
    Common::ParamPackage params({{"engine", "sdl"}, {"api", "controller"}, {"maptype", "all"}});
    params.Set("axis_x", 0);
    params.Set("axis_y", 1);
    params.Set("deadzone", 0.25f);
    stick = Input::CreateDevice<Input::AnalogDevice>(params.Serialize());
    poll_timer = new QTimer(this);
    connect(poll_timer, &QTimer::timeout, this, &Launcher::Poll);

    update_checker = new Updates::Checker(this);
    connect(update_checker, &Updates::Checker::Finished, this, &Launcher::UpdateChecked);

    qApp->installEventFilter(this);
}

Launcher::~Launcher() {
    StopBind();
}

void Launcher::UpdateFocusOrder() {
    // Top to bottom, so up and down walk the options of the open tab.
    std::vector<QWidget*> order = {update_get, browse, tabs->tabBar(), volume, language,
                                   unlock_galleries, unlock_fusion_mode, fusion_suit,
                                   import_saves, open_saves, update_startup, update_now,
                                   fullscreen, widescreen, camera_zoom,
                                   resolution, frame_rate, vsync, show_fps, menu_size, hud_size,
                                   hud_opacity, show_map, map_size, map_opacity, show_counters,
                                   show_abilities, panel_size, menu_backdrop, control_hints,
                                   gamepad_layout, keyboard_layout};
    for (const auto& bind_row : bind_rows) {
        order.push_back(bind_row.button);
    }
    order.insert(order.end(), mod_widgets.begin(), mod_widgets.end());
    order.insert(order.end(), {open_game_mods, refresh_game_mods, rando_download, rando_locate,
                               rando_preset, rando_edit, rando_permalink, rando_generate,
                               rando_open_seed});
    order.insert(order.end(), pack_widgets.begin(), pack_widgets.end());
    order.insert(order.end(), {open_mods, refresh_mods, preload_textures, dump_textures,
                               infinite_energy, infinite_missiles, infinite_aeion, quit, play});
    for (std::size_t i = 1; i < order.size(); ++i) {
        QWidget::setTabOrder(order[i - 1], order[i]);
    }
}

QWidget* Launcher::BuildDisplayTab() {
    auto* page = new QWidget;
    auto* grid = MakeGrid(page);
    fullscreen = AddCheck(grid, QStringLiteral("Fullscreen"),
                          [](bool value) { UISettings::values.fullscreen = value; });
    widescreen = AddCheck(grid, QStringLiteral("Widescreen (16:9)"), [this](bool value) {
        EditPlayer([value](PlayerSettings& p) { p.widescreen = value; });
    });
    camera_zoom = AddSlider(grid, QStringLiteral("Camera zoom"),
                            static_cast<int>(std::lround(CameraZoomMin * 100.f)),
                            static_cast<int>(std::lround(CameraZoomMax * 100.f)), Decimal,
                            [this](int value) {
                                EditPlayer([value](PlayerSettings& p) {
                                    p.camera_zoom = value / 100.f;
                                });
                            });
    camera_zoom->setToolTip(QStringLiteral("In play: below 1.00 shows more of the level, above "
                                           "it comes closer. The HUD keeps its size."));
    const int row = grid->rowCount();
    grid->addWidget(MakeLabel(QStringLiteral("Internal resolution")), row, 0);
    resolution = new QComboBox;
    for (const char* name : ResolutionNames) {
        resolution->addItem(QString::fromUtf8(name));
    }
    grid->addWidget(resolution, row, 1);
    connect(resolution, &QComboBox::currentIndexChanged, this, [](int index) {
        if (index >= 0) {
            Settings::values.resolution_factor = static_cast<u32>(index);
        }
    });
    const int rate_row = grid->rowCount();
    grid->addWidget(MakeLabel(QStringLiteral("Frame rate")), rate_row, 0);
    frame_rate = new QComboBox;
    for (const char* name : FrameRateNames) {
        frame_rate->addItem(QString::fromUtf8(name));
    }
    grid->addWidget(frame_rate, rate_row, 1);
    connect(frame_rate, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0 && index < static_cast<int>(FrameRate::Choices.size())) {
            EditPlayer([index](PlayerSettings& p) { p.frame_rate = FrameRate::Choices[index]; });
        }
    });
    vsync = AddCheck(grid, QStringLiteral("V-Sync"),
                     [](bool value) { Settings::values.use_vsync = value; });
    show_fps = AddCheck(grid, QStringLiteral("Show FPS"), [this](bool value) {
        EditPlayer([value](PlayerSettings& p) { p.show_fps = value; });
    });
    menu_size = AddSlider(grid, QStringLiteral("Menu size"), 70, 150, Decimal, [this](int value) {
        EditPlayer([value](PlayerSettings& p) { p.menu_scale = value / 100.f; });
    });
    grid->setRowStretch(grid->rowCount(), 1);
    return page;
}

QWidget* Launcher::BuildHudTab() {
    auto* page = new QWidget;
    auto* grid = MakeGrid(page);
    const auto player_slider = [&](const QString& label, int min, int max, float PlayerSettings::*field) {
        return AddSlider(grid, label, min, max, Decimal, [this, field](int value) {
            EditPlayer([field, value](PlayerSettings& p) { p.*field = value / 100.f; });
        });
    };
    const auto player_check = [&](const QString& label, bool PlayerSettings::*field) {
        return AddCheck(grid, label, [this, field](bool value) {
            EditPlayer([field, value](PlayerSettings& p) { p.*field = value; });
        });
    };
    hud_size = player_slider(QStringLiteral("HUD size"), 60, 160, &PlayerSettings::hud_scale);
    hud_opacity = player_slider(QStringLiteral("HUD opacity"), 30, 100, &PlayerSettings::hud_opacity);
    show_map = player_check(QStringLiteral("Map"), &PlayerSettings::show_minimap);
    map_size = player_slider(QStringLiteral("Map size"), 60, 180, &PlayerSettings::minimap_size);
    map_opacity =
        player_slider(QStringLiteral("Map opacity"), 30, 100, &PlayerSettings::minimap_opacity);
    show_counters =
        player_check(QStringLiteral("DNA and Metroid counters"), &PlayerSettings::show_counters);
    show_abilities = player_check(QStringLiteral("Aeion abilities"), &PlayerSettings::show_abilities);
    panel_size = player_slider(QStringLiteral("Menu panel size"), 50, 100, &PlayerSettings::panel_scale);
    menu_backdrop = player_slider(QStringLiteral("Title menu background"), 0, 100,
                                  &PlayerSettings::menu_backdrop);
    control_hints = player_check(QStringLiteral("Menu help text"), &PlayerSettings::control_hints);
    grid->setRowStretch(grid->rowCount(), 1);
    return page;
}

QWidget* Launcher::BuildControlsTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(22, 16, 22, 14);
    layout->setSpacing(10);
    profile = MakeLabel({}, "dim");
    layout->addWidget(profile);

    auto* presets = new QHBoxLayout;
    gamepad_layout = new QPushButton(QStringLiteral("Gamepad layout"));
    keyboard_layout = new QPushButton(QStringLiteral("Keyboard layout"));
    presets->addWidget(gamepad_layout);
    presets->addWidget(keyboard_layout);
    layout->addLayout(presets);
    connect(gamepad_layout, &QPushButton::clicked, this, [this] {
        StopBind();
        Controls::ApplyGamepadLayout();
        RefreshControls();
    });
    connect(keyboard_layout, &QPushButton::clicked, this, [this] {
        StopBind();
        Controls::ApplyKeyboardLayout();
        RefreshControls();
    });

    // One row per 3DS control; select one, then press the new button, key or stick.
    auto* list = new QWidget;
    list->setObjectName(QStringLiteral("bindlist"));
    auto* grid = new QGridLayout(list);
    grid->setContentsMargins(0, 0, 8, 0);
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(8);
    grid->setColumnMinimumWidth(0, 208);
    grid->setColumnStretch(1, 1);
    for (const auto& control : Controls::Rows) {
        const int row = grid->rowCount();
        grid->addWidget(MakeLabel(QString::fromUtf8(control.label)), row, 0);
        auto* button = new QPushButton;
        grid->addWidget(button, row, 1);
        const int id = control.id;
        connect(button, &QPushButton::clicked, this, [this, id] {
            if (bind_id == id) {
                StopBind();
                RefreshControls();
            } else {
                StartBind(id);
            }
        });
        bind_rows.push_back({id, button});
    }
    bindings = new QScrollArea;
    bindings->setWidget(list);
    bindings->setWidgetResizable(true);
    bindings->setFocusPolicy(Qt::NoFocus);
    bindings->viewport()->setAutoFillBackground(false);
    layout->addWidget(bindings, 1);

    layout->addWidget(MakeLabel(QStringLiteral("In game: F1 or Select settings     "
                                               "ZR (right trigger) usual, Ice or Grapple Beam     "
                                               "ZL (left trigger) Morph Ball"),
                                "dim"));
    layout->addWidget(MakeLabel(QStringLiteral("Tab, or L3 in menus, touch screen navigation     "
                                               "F11 fullscreen"),
                                "dim"));
    return page;
}

QWidget* Launcher::BuildGameTab() {
    auto* page = new QWidget;
    auto* grid = MakeGrid(page);
    volume = AddSlider(grid, QStringLiteral("Volume"), 0, 100, Percent, [](int value) {
        Settings::values.volume = static_cast<float>(value) / 100.f;
    });
    // The game's text follows the 3DS system language, read when it starts.
    const int language_row = grid->rowCount();
    grid->addWidget(MakeLabel(QStringLiteral("Language")), language_row, 0);
    language = new QComboBox;
    for (const auto& entry : Languages) {
        language->addItem(QString::fromUtf8(entry.name));
    }
    language->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    language->setToolTip(QStringLiteral("The game's text. The North American game has these six; "
                                        "it applies when the game starts."));
    grid->addWidget(language, language_row, 1, Qt::AlignLeft);
    connect(language, &QComboBox::currentIndexChanged, this, [](int index) {
        if (index < 0 || index >= static_cast<int>(Languages.size())) {
            return;
        }
        const Language& chosen = Languages[index];
        SetRegionFor(chosen);
        const auto cfg = Service::CFG::GetModule(Core::System::GetInstance());
        if (cfg && cfg->GetSystemLanguage() != chosen.value) {
            cfg->SetSystemLanguage(chosen.value);
            cfg->UpdateConfigNANDSavegame();
        }
    });
    // Extras otherwise given by amiibo (msr/cheats.h).
    const auto player_check = [&](const QString& label, bool PlayerSettings::*field) {
        return AddCheck(grid, label, [this, field](bool value) {
            EditPlayer([field, value](PlayerSettings& p) { p.*field = value; });
        });
    };
    unlock_galleries =
        player_check(QStringLiteral("Unlock amiibo galleries"), &PlayerSettings::unlock_galleries);
    unlock_fusion_mode =
        player_check(QStringLiteral("Unlock Fusion Mode"), &PlayerSettings::unlock_fusion_mode);
    fusion_suit =
        player_check(QStringLiteral("Fusion suit, any difficulty"), &PlayerSettings::fusion_suit);
    fusion_note = MakeLabel(QStringLiteral("Off while a randomizer mod is on: it has a Fusion suit "
                                           "option of its own."),
                            "dim");
    fusion_note->setWordWrap(true);
    fusion_note->hide();
    grid->addWidget(fusion_note, grid->rowCount(), 1);
    // Saves from a 3DS (Checkpoint, JKSM) or another emulator.
    const int saves_row = grid->rowCount();
    grid->addWidget(MakeLabel(QStringLiteral("Saves")), saves_row, 0);
    import_saves = new QPushButton(QStringLiteral("Import saves..."));
    open_saves = new QPushButton(QStringLiteral("Open saves folder"));
    auto* saves = new QHBoxLayout;
    saves->setSpacing(12);
    saves->addWidget(import_saves);
    saves->addWidget(open_saves);
    saves->addStretch(1);
    grid->addLayout(saves, saves_row, 1);
    connect(import_saves, &QPushButton::clicked, this, &Launcher::ImportSaves);
    connect(open_saves, &QPushButton::clicked, this, [] {
        const auto info = GameFolder::Probe(GameFolder::ActiveRom());
        Saves::OpenFolder(info.Ok() ? info.program_id : GameFolder::UsaProgramId);
    });
    // New versions on GitHub (msr_updates.h).
    const int updates_row = grid->rowCount();
    grid->addWidget(MakeLabel(QStringLiteral("Updates")), updates_row, 0);
    update_startup = new QCheckBox(QStringLiteral("Check when the launcher opens"));
    update_now = new QPushButton(QStringLiteral("Check now"));
    auto* updates = new QHBoxLayout;
    updates->setSpacing(16);
    updates->addWidget(update_startup);
    updates->addWidget(update_now);
    updates->addStretch(1);
    grid->addLayout(updates, updates_row, 1);
    update_status = MakeLabel(QStringLiteral("You have %1 %2.")
                                  .arg(QLatin1String(MSR::ProjectName),
                                       QLatin1String(MSR::VersionLabel)),
                              "dim");
    update_status->setWordWrap(true);
    grid->addWidget(update_status, grid->rowCount(), 1);
    connect(update_startup, &QCheckBox::toggled, this,
            [](bool on) { Updates::SetCheckAtStartup(on); });
    connect(update_now, &QPushButton::clicked, this, &Launcher::CheckForUpdates);
    auto* note = MakeLabel(QStringLiteral("Quick save, quick load and restart are in the in-game "
                                          "menu (F1 or Select)."),
                           "dim");
    note->setWordWrap(true);
    grid->addWidget(note, grid->rowCount(), 0, 1, 2);
    grid->setRowStretch(grid->rowCount(), 1);
    return page;
}

QWidget* Launcher::BuildCheatsTab() {
    auto* page = new QWidget;
    auto* grid = MakeGrid(page);
    const auto player_check = [&](const QString& label, bool PlayerSettings::*field) {
        return AddCheck(grid, label, [this, field](bool value) {
            EditPlayer([field, value](PlayerSettings& p) { p.*field = value; });
        });
    };
    infinite_energy = player_check(QStringLiteral("Infinite energy"), &PlayerSettings::infinite_energy);
    infinite_missiles =
        player_check(QStringLiteral("Unlimited missiles"), &PlayerSettings::infinite_missiles);
    infinite_aeion = player_check(QStringLiteral("Unlimited Aeion"), &PlayerSettings::infinite_aeion);
    auto* note = MakeLabel(QStringLiteral("Kept full every frame. Missiles include Super Missiles "
                                          "and Power Bombs. A save keeps what you have when you "
                                          "save."),
                           "dim");
    note->setWordWrap(true);
    grid->addWidget(note, grid->rowCount(), 0, 1, 2);
    grid->setRowStretch(grid->rowCount(), 1);
    return page;
}

QWidget* Launcher::BuildModsTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(22, 16, 22, 14);
    layout->setSpacing(10);
    auto* about = MakeLabel(QStringLiteral("Mods made for 3DS emulators: each folder holds romfs, "
                                           "romfs_ext, exefs, code.ips or exheader.bin. They "
                                           "load when the game starts; where mods change the "
                                           "same file, the first one listed wins. Randomizer "
                                           "seeds are made in the Randomizer tab, or exported "
                                           "from Randovania for Azahar or to a folder here."),
                            "dim");
    about->setWordWrap(true);
    layout->addWidget(about);
    mods_area = MakeModList(mod_list);
    layout->addWidget(mods_area, 1);

    auto* buttons = new QHBoxLayout;
    open_game_mods = new QPushButton(QStringLiteral("Open the mods folder"));
    refresh_game_mods = new QPushButton(QStringLiteral("Refresh"));
    buttons->addWidget(open_game_mods, 1);
    buttons->addWidget(refresh_game_mods);
    layout->addLayout(buttons);
    connect(open_game_mods, &QPushButton::clicked, this, [] { Mods::OpenModFolder(); });
    connect(refresh_game_mods, &QPushButton::clicked, this, [this] { RefreshMods(); });
    return page;
}

QWidget* Launcher::BuildTexturesTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(22, 16, 22, 14);
    layout->setSpacing(10);
    layout->addWidget(MakeLabel(QStringLiteral("Texture packs load when the game starts. Where "
                                               "packs replace the same texture, the first one "
                                               "listed wins."),
                                "dim"));
    packs = MakeModList(pack_list);
    layout->addWidget(packs, 1);

    auto* buttons = new QHBoxLayout;
    open_mods = new QPushButton(QStringLiteral("Open the texture packs folder"));
    refresh_mods = new QPushButton(QStringLiteral("Refresh"));
    buttons->addWidget(open_mods, 1);
    buttons->addWidget(refresh_mods);
    layout->addLayout(buttons);
    connect(open_mods, &QPushButton::clicked, this, [] { Mods::OpenFolder(); });
    connect(refresh_mods, &QPushButton::clicked, this, [this] { RefreshMods(); });

    auto* options = new QWidget;
    auto* grid = MakeGrid(options);
    grid->setContentsMargins(0, 4, 0, 0);
    preload_textures = AddCheck(grid, QStringLiteral("Preload textures"),
                                [](bool value) { Settings::values.preload_textures = value; });
    preload_textures->setToolTip(QStringLiteral("Loads every pack texture at boot: no stutter "
                                                "when new ones appear, more memory and a longer "
                                                "start."));
    dump_textures = AddCheck(grid, QStringLiteral("Dump game textures"),
                             [](bool value) { Settings::values.dump_textures = value; });
    dump_textures->setToolTip(QStringLiteral("Saves the textures the game shows to "
                                             "user/dump/textures/, named as packs expect: edit "
                                             "them to make your own pack."));
    layout->addWidget(options);
    return page;
}

QWidget* Launcher::BuildRandomizerTab() {
    rando_page = new QWidget;
    auto* grid = MakeGrid(rando_page);
    grid->setVerticalSpacing(10);

    // Randovania itself: downloaded here, or the player's own copy.
    const int program_row = grid->rowCount();
    grid->addWidget(MakeLabel(QStringLiteral("Randovania")), program_row, 0, Qt::AlignTop);
    auto* program = new QVBoxLayout;
    program->setSpacing(8);
    rando_state = MakeLabel({});
    program->addWidget(rando_state);
    auto* program_buttons = new QHBoxLayout;
    program_buttons->setSpacing(12);
    rando_download = new QPushButton;
    rando_locate = new QPushButton(QStringLiteral("Use my own copy..."));
    program_buttons->addWidget(rando_download);
    program_buttons->addWidget(rando_locate);
    program_buttons->addStretch(1);
    program->addLayout(program_buttons);
    grid->addLayout(program, program_row, 1);

    const int preset_row = grid->rowCount();
    grid->addWidget(MakeLabel(QStringLiteral("Preset")), preset_row, 0);
    auto* preset = new QHBoxLayout;
    preset->setSpacing(12);
    rando_preset = new QComboBox;
    rando_preset->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    rando_edit = new QPushButton(QStringLiteral("Make presets in Randovania"));
    rando_edit->setToolTip(QStringLiteral("Opens Randovania: presets you make there for Samus "
                                          "Returns are listed here."));
    preset->addWidget(rando_preset, 1);
    preset->addWidget(rando_edit);
    grid->addLayout(preset, preset_row, 1);
    rando_preset_info = MakeLabel({}, "dim");
    rando_preset_info->setWordWrap(true);
    grid->addWidget(rando_preset_info, grid->rowCount(), 1);

    const int permalink_row = grid->rowCount();
    grid->addWidget(MakeLabel(QStringLiteral("Shared seed")), permalink_row, 0);
    rando_permalink = new QLineEdit;
    rando_permalink->setPlaceholderText(
        QStringLiteral("Optional: paste a permalink to play the same seed as someone else"));
    rando_permalink->setClearButtonEnabled(true);
    grid->addWidget(rando_permalink, permalink_row, 1);

    const int actions_row = grid->rowCount();
    auto* actions = new QHBoxLayout;
    actions->setSpacing(12);
    rando_generate = new QPushButton;
    rando_open_seed = new QPushButton(QStringLiteral("Open the seed in Randovania"));
    rando_open_seed->setToolTip(QStringLiteral("The seed that is on: its spoiler log, and "
                                               "Randovania's map tracker."));
    actions->addWidget(rando_generate);
    actions->addWidget(rando_open_seed);
    actions->addStretch(1);
    grid->addLayout(actions, actions_row, 1);
    rando_status = MakeLabel({});
    rando_status->setWordWrap(true);
    rando_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(rando_status, grid->rowCount(), 1);

    auto* note = MakeLabel(QStringLiteral("Randovania shuffles where the items are; each seed "
                                          "becomes a game mod, turned on in the Mods tab with "
                                          "other seeds off. Start a new file for it: while it is "
                                          "on, other files do not load."),
                           "dim");
    note->setWordWrap(true);
    grid->addWidget(note, grid->rowCount(), 0, 1, 2);
    grid->setRowStretch(grid->rowCount(), 1);

    rando_task = new Randovania::Task(this);
    connect(rando_task, &Randovania::Task::Progress, this,
            [this](const QString& text) { ShowRandomizerStatus(text, "#dcf0ff"); });
    connect(rando_task, &Randovania::Task::Finished, this, &Launcher::RandomizerFinished);
    connect(rando_download, &QPushButton::clicked, this, [this] {
        if (rando_task->Running() == Randovania::Task::Job::Download) {
            rando_task->Cancel();
            return;
        }
        rando_task->Download();
        RefreshRandomizer();
        UpdateRom();
    });
    connect(rando_locate, &QPushButton::clicked, this, [this] {
#ifdef _WIN32
        const QString filter = QStringLiteral("Randovania (randovania.exe)");
#else
        const QString filter = QStringLiteral("Randovania (randovania)");
#endif
        const QString own = Randovania::OwnProgram();
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Select the randovania program of your Randovania"),
            own.isEmpty() ? QDir::homePath() : QFileInfo(own).absolutePath(), filter);
        if (!path.isEmpty()) {
            Randovania::SetOwnProgram(path);
            ShowRandomizerStatus({}, "#dcf0ff");
            RefreshRandomizer();
        }
    });
    connect(rando_preset, &QComboBox::currentIndexChanged, this, [this](int index) {
        rando_preset_info->setText(index >= 0 && index < static_cast<int>(rando_presets.size())
                                       ? rando_presets[index].description
                                       : QString{});
    });
    connect(rando_edit, &QPushButton::clicked, this, [this] {
        if (!Randovania::OpenWindow()) {
            ShowRandomizerStatus(QStringLiteral("Randovania could not start."), "#ff9a8a");
        }
    });
    connect(rando_permalink, &QLineEdit::textChanged, this, [this] { RefreshRandomizer(); });
    connect(rando_generate, &QPushButton::clicked, this, &Launcher::GenerateSeed);
    connect(rando_open_seed, &QPushButton::clicked, this, [this] {
        if (!Randovania::OpenWindow(Randovania::EnabledSeedLog())) {
            ShowRandomizerStatus(QStringLiteral("Randovania could not start."), "#ff9a8a");
        }
    });
    return rando_page;
}

void Launcher::RefreshRandomizer() {
    using Job = Randovania::Task::Job;
    const Job job = rando_task->Running();
    const QString program = Randovania::Program();
    const QString own = Randovania::OwnProgram();
    const QString version = Randovania::DownloadedVersion();
    if (!own.isEmpty()) {
        rando_state->setText(rando_state->fontMetrics().elidedText(
            QStringLiteral("Your copy: %1").arg(QDir::toNativeSeparators(own)), Qt::ElideMiddle,
            560));
        rando_state->setToolTip(QDir::toNativeSeparators(own));
    } else if (!version.isEmpty()) {
        rando_state->setText(QStringLiteral("Randovania %1, downloaded").arg(version));
        rando_state->setToolTip(
            QDir::toNativeSeparators(GameFolder::User() + QStringLiteral("randovania")));
    } else {
        rando_state->setText(QStringLiteral("Not set up: download it (about 200 MB, from GitHub) "
                                            "or use your own copy."));
        rando_state->setToolTip({});
    }
    rando_download->setText(job == Job::Download ? QStringLiteral("Stop")
                            : version.isEmpty()  ? QStringLiteral("Download Randovania")
                                                 : QStringLiteral("Check for an update"));
    rando_download->setEnabled(job != Job::Generate);
    rando_locate->setEnabled(job == Job::None);

    // The presets, keeping the one chosen (or the one last generated with).
    QString keep = Randovania::LastPreset();
    if (const int index = rando_preset->currentIndex();
        index >= 0 && index < static_cast<int>(rando_presets.size())) {
        keep = rando_presets[index].file.isEmpty() ? rando_presets[index].name
                                                   : rando_presets[index].file;
    }
    rando_presets = Randovania::Presets();
    {
        const QSignalBlocker block(rando_preset);
        rando_preset->clear();
        int chosen = 0;
        for (std::size_t i = 0; i < rando_presets.size(); ++i) {
            const auto& entry = rando_presets[i];
            rando_preset->addItem(entry.file.isEmpty() ? entry.name + QStringLiteral(" (included)")
                                                       : entry.name);
            if ((entry.file.isEmpty() ? entry.name : entry.file) == keep) {
                chosen = static_cast<int>(i);
            }
        }
        rando_preset->setCurrentIndex(chosen);
    }
    rando_preset_info->setText(rando_presets.empty()
                                   ? QString{}
                                   : rando_presets[rando_preset->currentIndex()].description);
    // A permalink holds its own preset.
    const bool permalink = !rando_permalink->text().trimmed().isEmpty();
    rando_preset->setEnabled(job == Job::None && !permalink);
    rando_edit->setEnabled(!program.isEmpty());
    rando_permalink->setEnabled(job == Job::None);
    rando_generate->setText(job == Job::Generate ? QStringLiteral("Stop")
                            : permalink          ? QStringLiteral("Generate the shared seed")
                                                 : QStringLiteral("Generate a seed"));
    rando_generate->setEnabled(job == Job::Generate || (job == Job::None && !program.isEmpty()));
    rando_open_seed->setEnabled(job == Job::None && !program.isEmpty() &&
                                !Randovania::EnabledSeedLog().isEmpty());
}

void Launcher::GenerateSeed() {
    if (rando_task->Running() == Randovania::Task::Job::Generate) {
        rando_task->Cancel();
        return;
    }
    const QString rom = GameFolder::ActiveRom();
    const auto info = rom.isEmpty() ? GameFolder::RomInfo{} : GameFolder::Probe(rom);
    if (rom.isEmpty() || !info.Ok()) {
        ShowRandomizerStatus(QStringLiteral("Choose your game dump first (Browse, at the top)."),
                             "#ff9a8a");
        return;
    }
    if (info.encrypted) {
        ShowRandomizerStatus(
            QStringLiteral("Randovania needs a decrypted dump; this one is encrypted."), "#ff9a8a");
        return;
    }
    const int index = rando_preset->currentIndex();
    if (index < 0 || index >= static_cast<int>(rando_presets.size())) {
        return;
    }
    rando_task->Generate(rando_presets[index], rando_permalink->text().trimmed(), rom);
    RefreshRandomizer();
    UpdateRom();
}

void Launcher::RandomizerFinished(const QString& error, const QString& name,
                                  const QString& detail) {
    if (!error.isEmpty()) {
        ShowRandomizerStatus(
            error + QStringLiteral(" (More in %1, in the user folder.)")
                        .arg(QDir::toNativeSeparators(QStringLiteral("randovania/last-run.log"))),
            "#ff9a8a");
        rando_status->setToolTip(
            QDir::toNativeSeparators(GameFolder::User() + QStringLiteral("randovania/last-run.log")));
    } else if (name.startsWith(QStringLiteral("Randomizer - "))) {
        // The new seed is the one that loads.
        for (const auto& mod : Mods::GameMods()) {
            if (mod.name != name && mod.enabled && Mods::IsRandomizer(mod)) {
                Mods::SetModEnabled(mod.name, false);
            }
        }
        Mods::SetModEnabled(name, true);
        rando_permalink->clear();
        RefreshMods();
        ShowRandomizerStatus(QStringLiteral("Seed ready: %1. It is on in the Mods tab: press "
                                            "Play and start a new file.")
                                 .arg(detail),
                             "#86e39a");
    } else {
        ShowRandomizerStatus(detail == QStringLiteral("already")
                                 ? QStringLiteral("Randovania %1 is the latest.").arg(name)
                                 : QStringLiteral("Randovania %1 is ready.").arg(name),
                             "#86e39a");
    }
    RefreshRandomizer();
    UpdateRom();
}

void Launcher::ShowRandomizerStatus(const QString& text, const char* color) {
    rando_status->setText(text);
    rando_status->setToolTip({});
    rando_status->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(color)));
}

void Launcher::RefreshMods() {
    std::vector<ModRow> rows;
    for (const auto& mod : Mods::GameMods()) {
        rows.push_back({mod.name, Mods::Describe(mod), mod.enabled});
    }
    FillModList(mod_list, mod_widgets, rows,
                QStringLiteral("No mods yet. Put each mod in its own folder in %1")
                    .arg(QDir::toNativeSeparators(Mods::GameModFolder())),
                [this](const QString& name, bool enabled) {
                    Mods::SetModEnabled(name, enabled);
                    UpdateFusionSuit();
                },
                Mods::MoveMod);
    rows.clear();
    for (const auto& pack : Mods::TexturePacks()) {
        rows.push_back({pack.name, Mods::Describe(pack), pack.enabled});
    }
    FillModList(pack_list, pack_widgets, rows,
                QStringLiteral("No texture packs yet. Put each pack in its own folder in %1")
                    .arg(QDir::toNativeSeparators(Mods::TexturePackFolder())),
                Mods::SetEnabled, Mods::Move);
    preload_textures->setChecked(Settings::values.preload_textures.GetValue());
    dump_textures->setChecked(Settings::values.dump_textures.GetValue());
    UpdateFusionSuit();
    UpdateFocusOrder();
}

void Launcher::UpdateFusionSuit() {
    // A randomizer has a Fusion suit option of its own, read where this one rewrites the game
    // (msr/cheats.h): with one on, this one stays off.
    const bool randomizer = Mods::RandomizerEnabled();
    const QSignalBlocker block(fusion_suit);
    fusion_suit->setEnabled(!randomizer);
    fusion_suit->setChecked(!randomizer && GetOverlay().GetPlayerSettings().fusion_suit);
    fusion_note->setVisible(randomizer);
}

void Launcher::FillModList(QWidget* list, std::vector<QWidget*>& widgets,
                           const std::vector<ModRow>& entries, const QString& empty_text,
                           std::function<void(const QString&, bool)> enable,
                           std::function<void(const QString&, int)> move) {
    auto* rows = static_cast<QGridLayout*>(list->layout());
    while (QLayoutItem* item = rows->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    widgets.clear();

    if (entries.empty()) {
        auto* empty = MakeLabel(empty_text);
        empty->setWordWrap(true);
        empty->setTextInteractionFlags(Qt::TextSelectableByMouse);
        rows->addWidget(empty, 0, 0, 1, 4);
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const int row = static_cast<int>(i);
        auto* box = new QCheckBox(entry.name);
        box->setChecked(entry.enabled);
        auto* detail = MakeLabel(entry.detail, "dim");
        auto* up = new QPushButton(QStringLiteral("Up"));
        auto* down = new QPushButton(QStringLiteral("Down"));
        up->setEnabled(i > 0);
        down->setEnabled(i + 1 < entries.size());
        rows->addWidget(box, row, 0);
        rows->addWidget(detail, row, 1);
        rows->addWidget(up, row, 2);
        rows->addWidget(down, row, 3);
        const QString name = entry.name;
        connect(box, &QCheckBox::toggled, this, [enable, name](bool on) { enable(name, on); });
        const auto shift = [this, move, name, list = &widgets](int direction, bool focus_up) {
            move(name, direction);
            // Rebuilt after the click returns; the focus follows the entry.
            QTimer::singleShot(0, this, [this, name, focus_up, list] {
                RefreshMods();
                for (std::size_t j = 0; j + 2 < list->size(); j += 3) {
                    if (static_cast<QCheckBox*>((*list)[j])->text() == name) {
                        QWidget* target = (*list)[j + (focus_up ? 1 : 2)];
                        (target->isEnabled() ? target : (*list)[j])->setFocus(Qt::OtherFocusReason);
                    }
                }
            });
        };
        connect(up, &QPushButton::clicked, this, [shift] { shift(-1, true); });
        connect(down, &QPushButton::clicked, this, [shift] { shift(1, false); });
        widgets.insert(widgets.end(), {box, up, down});
    }
    rows->setRowStretch(rows->rowCount(), 1);
}

QCheckBox* Launcher::AddCheck(QGridLayout* grid, const QString& label,
                              std::function<void(bool)> changed) {
    const int row = grid->rowCount();
    grid->addWidget(MakeLabel(label), row, 0);
    auto* box = new QCheckBox;
    box->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    grid->addWidget(box, row, 1, Qt::AlignLeft);
    connect(box, &QCheckBox::toggled, this, std::move(changed));
    return box;
}

QSlider* Launcher::AddSlider(QGridLayout* grid, const QString& label, int min, int max,
                             std::function<QString(int)> format,
                             std::function<void(int)> changed) {
    const int row = grid->rowCount();
    grid->addWidget(MakeLabel(label), row, 0);
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(min, max);
    slider->setSingleStep(5);
    slider->setPageStep(10);
    auto* value = MakeLabel(format(slider->value()));
    value->setMinimumWidth(56);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* cell = new QHBoxLayout;
    cell->setSpacing(14);
    cell->addWidget(slider, 1);
    cell->addWidget(value);
    grid->addLayout(cell, row, 1);
    connect(slider, &QSlider::valueChanged, this,
            [value, format = std::move(format), changed = std::move(changed)](int v) {
                value->setText(format(v));
                changed(v);
            });
    return slider;
}

void Launcher::EditPlayer(const std::function<void(PlayerSettings&)>& edit) {
    auto player = GetOverlay().GetPlayerSettings();
    edit(player);
    GetOverlay().SetPlayerSettings(player);
}

void Launcher::Open() {
    Load();
    UpdateRom();
    RefreshControls();
    // Once per run: the launcher also comes back when the game stops with an error.
    if (!update_checked && Updates::CheckAtStartup()) {
        update_checked = true;
        CheckForUpdates();
    }
    show();
    (play->isEnabled() ? play : browse)->setFocus(Qt::OtherFocusReason);
}

void Launcher::Load() {
    const auto p = GetOverlay().GetPlayerSettings();
    const auto percent = [](float value) { return static_cast<int>(std::lround(value * 100.f)); };
    fullscreen->setChecked(UISettings::values.fullscreen.GetValue());
    widescreen->setChecked(p.widescreen);
    camera_zoom->setValue(percent(p.camera_zoom));
    resolution->setCurrentIndex(std::clamp(static_cast<int>(Settings::values.resolution_factor.GetValue()),
                                           0, resolution->count() - 1));
    const auto rate = std::find(FrameRate::Choices.begin(), FrameRate::Choices.end(), p.frame_rate);
    frame_rate->setCurrentIndex(rate == FrameRate::Choices.end()
                                    ? 1
                                    : static_cast<int>(rate - FrameRate::Choices.begin()));
    if (const auto cfg = Service::CFG::GetModule(Core::System::GetInstance())) {
        // Another language reads as English, which the game shows for it.
        const auto current = cfg->GetSystemLanguage();
        const auto it =
            std::find_if(Languages.begin(), Languages.end(),
                         [current](const Language& entry) { return entry.value == current; });
        const QSignalBlocker block(language);
        language->setCurrentIndex(
            it == Languages.end() ? 0 : static_cast<int>(it - Languages.begin()));
        // A European language set some other way needs the region too.
        if (it != Languages.end() && it->european) {
            SetRegionFor(*it);
        }
    }
    vsync->setChecked(Settings::values.use_vsync.GetValue());
    show_fps->setChecked(p.show_fps);
    menu_size->setValue(percent(p.menu_scale));
    hud_size->setValue(percent(p.hud_scale));
    hud_opacity->setValue(percent(p.hud_opacity));
    show_map->setChecked(p.show_minimap);
    map_size->setValue(percent(p.minimap_size));
    map_opacity->setValue(percent(p.minimap_opacity));
    show_counters->setChecked(p.show_counters);
    show_abilities->setChecked(p.show_abilities);
    panel_size->setValue(percent(p.panel_scale));
    menu_backdrop->setValue(percent(p.menu_backdrop));
    control_hints->setChecked(p.control_hints);
    volume->setValue(percent(Settings::values.volume.GetValue()));
    unlock_galleries->setChecked(p.unlock_galleries);
    unlock_fusion_mode->setChecked(p.unlock_fusion_mode);
    fusion_suit->setChecked(p.fusion_suit);
    infinite_energy->setChecked(p.infinite_energy);
    infinite_missiles->setChecked(p.infinite_missiles);
    infinite_aeion->setChecked(p.infinite_aeion);
    RefreshMods();
    RefreshRandomizer();
    const QSignalBlocker block(update_startup);
    update_startup->setChecked(Updates::CheckAtStartup());
}

void Launcher::CheckForUpdates() {
    update_now->setEnabled(false);
    update_status->setText(QStringLiteral("Checking GitHub for a newer version..."));
    update_checker->Start();
}

void Launcher::UpdateChecked(const QString& error, const QString& version, const QString& title,
                             const QString& url) {
    update_now->setEnabled(true);
    const QString current =
        QStringLiteral("%1 %2").arg(QLatin1String(MSR::ProjectName), QLatin1String(MSR::VersionLabel));
    if (!error.isEmpty()) {
        update_status->setText(QStringLiteral("Could not check: %1").arg(error));
        return;
    }
    if (version.isEmpty()) {
        update_status->setText(QStringLiteral("%1 is the latest version.").arg(current));
        return;
    }
    const QString name =
        title.isEmpty() ? QStringLiteral("%1 %2").arg(QLatin1String(MSR::ProjectName), version)
                        : title;
    update_url = url;
    update_text->setText(QStringLiteral("%1 is out").arg(name));
    update_banner->show();
    update_status->setText(
        QStringLiteral("%1 is out; you have %2. Its download page opens from the top of the "
                       "launcher.")
            .arg(name, current));
}

void Launcher::UpdateRom(const QString& error) {
    const QString rom = GameFolder::ActiveRom();
    const auto info = rom.isEmpty() ? GameFolder::RomInfo{} : GameFolder::Probe(rom);
    const bool ok = !rom.isEmpty() && info.Ok();
    rom_path->setText(QDir::toNativeSeparators(rom));

    QString text;
    QString color = QStringLiteral("#dcf0ff");
    if (!error.isEmpty()) {
        text = error;
        color = QStringLiteral("#ff9a8a");
    } else if (ok) {
        text = QStringLiteral("Ready: Metroid: Samus Returns, %1 (%2)")
                   .arg(info.Region(), info.product_code);
        color = QStringLiteral("#86e39a");
    } else if (rom.isEmpty()) {
        text = QStringLiteral("The game is not included. Choose your own decrypted dump of "
                              "Metroid: Samus Returns (.3ds or .cci); it stays where it is, and "
                              "its location is remembered.");
    } else {
        text = info.error;
        color = QStringLiteral("#ff9a8a");
    }
    rom_status->setText(text);
    rom_status->setStyleSheet(QStringLiteral("color: %1;").arg(color));
    // Not while a seed is being written into the mods folder.
    play->setEnabled(ok && rando_task->Running() == Randovania::Task::Job::None);

    QPixmap pixmap;
    if (ok) {
        if (const auto loader = Loader::GetLoader(rom.toStdString())) {
            std::vector<u8> smdh;
            if (loader->ReadIcon(smdh) == Loader::ResultStatus::Success &&
                Loader::IsValidSMDH(smdh)) {
                pixmap = GetQPixmapFromSMDH(smdh);
            }
        }
    }
    if (!pixmap.isNull()) {
        const qreal ratio = devicePixelRatioF();
        pixmap = pixmap.scaled(QSize(72, 72) * ratio, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pixmap.setDevicePixelRatio(ratio);
        icon->setPixmap(pixmap);
    }
    icon->setVisible(!pixmap.isNull());
}

void Launcher::Browse() {
    QString start = GameFolder::Root();
    if (const QString rom = GameFolder::ActiveRom(); !rom.isEmpty()) {
        start = QFileInfo(rom).absolutePath();
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select your Metroid: Samus Returns dump"), start,
        QStringLiteral("3DS game dumps (*.3ds *.cci *.cxi);;All files (*)"));
    if (!path.isEmpty()) {
        ChooseRom(path);
    }
}

void Launcher::ImportSaves() {
    const auto info = GameFolder::Probe(GameFolder::ActiveRom());
    const u64 program_id = info.Ok() ? info.program_id : GameFolder::UsaProgramId;
    const QString title = QStringLiteral("Import saves");
    const QString path = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select the folder with your Samus Returns saves"), QDir::homePath());
    if (path.isEmpty()) {
        return;
    }
    const auto found = Saves::Find(path, program_id);
    if (found.empty()) {
        QMessageBox::warning(
            this, title,
            // Not naming Azahar: the input bridge renames it in every message box.
            QStringLiteral("No Samus Returns saves were found in %1.\n\nChoose a Checkpoint or "
                           "JKSM backup of the game, or the folder of a 3DS emulator (Citra, "
                           "Lime3DS...) that has played it.")
                .arg(QDir::toNativeSeparators(path)));
        return;
    }
    const auto& save = found.front();
    QString text = QStringLiteral("Import %1 save file%2 from:\n%3\n(last saved %4)")
                       .arg(save.files)
                       .arg(save.files == 1 ? QString{} : QStringLiteral("s"))
                       .arg(QDir::toNativeSeparators(save.folder))
                       .arg(QLocale().toString(save.modified, QLocale::ShortFormat));
    if (found.size() == 2) {
        text += QStringLiteral("\n\nAnother save folder is there too; to import it instead, "
                               "choose its folder.");
    } else if (found.size() > 2) {
        text += QStringLiteral("\n\n%1 other save folders are there too; to import one of them, "
                               "choose its folder.")
                    .arg(found.size() - 1);
    }
    text += QStringLiteral("\n\nThese replace all three files here. The current ones are "
                           "kept in user/msr/save-backups/.");
    if (QMessageBox::question(this, title, text) != QMessageBox::Yes) {
        return;
    }
    QString backup;
    const QString error = Saves::Import(save, program_id, &backup);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, title, error);
        return;
    }
    QMessageBox::information(
        this, title,
        backup.isEmpty() ? QStringLiteral("Saves imported.")
                         : QStringLiteral("Saves imported. The previous ones are in:\n%1")
                               .arg(backup));
}

void Launcher::ChooseRom(const QString& path) {
    const auto info = GameFolder::Probe(path);
    if (!info.Ok()) {
        UpdateRom(QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), info.error));
        return;
    }
    GameFolder::RememberRom(path);
    GameFolder::SetActiveRom(path);
    UpdateRom();
    play->setFocus(Qt::OtherFocusReason);
}

void Launcher::RefreshControls() {
    profile->setText(QStringLiteral("Profile: %1").arg(
        QString::fromStdString(Settings::values.current_input_profile.name)));
    for (const auto& row : bind_rows) {
        QString text;
        if (row.id == bind_id) {
            text = row.id >= Controls::AnalogBase
                       ? QStringLiteral("Move a stick...   (Esc cancels)")
                       : QStringLiteral("Press a button or key...   (Esc cancels)");
        } else {
            text = QString::fromStdString(Controls::BindingText(row.id));
        }
        row.button->setText(text.replace(QLatin1Char('&'), QStringLiteral("&&")));
    }
    gamepad_layout->setEnabled(Controls::ControllerConnected());
}

void Launcher::StartBind(int id) {
    StopBind();
    bind_id = id;
    bind_started = false;
    bind_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    RefreshControls();
}

void Launcher::FinishBind(const std::string& param) {
    const int id = bind_id;
    StopBind();
    Controls::Bind(id, param);
    RefreshControls();
}

void Launcher::StopBind() {
    for (auto& poller : pollers) {
        poller->Stop();
    }
    pollers.clear();
    bind_id = -1;
    bind_started = false;
}

void Launcher::Poll() {
    // The card re-centres as tabs of other heights open, and the window can be resized: the ring
    // follows its option wherever the layout moves it.
    FollowFocus();
    const auto time = std::chrono::steady_clock::now();
    std::array<bool, ButtonCount> now{};
    bool any = false;
    for (int i = 0; i < ButtonCount; ++i) {
        now[i] = buttons[i] && buttons[i]->GetStatus();
        any |= now[i];
    }
    float x = 0.f, y = 0.f;
    if (stick) {
        std::tie(x, y) = stick->GetStatus();
    }
    const auto before = previous;
    previous = now;
    const auto pressed = [&](int button) { return now[button] && !before[button]; };

    if (bind_id >= 0) {
        // Rebinding: wait for the button that chose the row to be released, then take the
        // next gamepad input. Keys arrive through eventFilter.
        if (time > bind_deadline) {
            StopBind();
            RefreshControls();
        } else if (!bind_started) {
            if (!any) {
                pollers = InputCommon::Polling::GetPollers(
                    bind_id >= Controls::AnalogBase ? InputCommon::Polling::DeviceType::Analog
                                                    : InputCommon::Polling::DeviceType::Button);
                for (auto& poller : pollers) {
                    poller->Start();
                }
                bind_started = true;
            }
        } else {
            for (auto& poller : pollers) {
                if (const auto param = poller->GetNextInput(); param.Has("engine")) {
                    FinishBind(param.Serialize());
                    break;
                }
            }
        }
        return;
    }
    if (time >= next_refresh) {
        // Gamepads come and go; the profile may follow them (GMainWindow::AutoConfigureMsrController).
        next_refresh = time + std::chrono::seconds(1);
        RefreshControls();
    }
    if (!window()->isActiveWindow()) {
        return;
    }

    // D-pad or stick: up and down move between options, left and right change them. Holding
    // repeats.
    const int dx = now[DpadLeft] ? -1 : now[DpadRight] ? 1 : x < -.5f ? -1 : x > .5f ? 1 : 0;
    const int dy = now[DpadUp] ? -1 : now[DpadDown] ? 1 : y > .5f ? -1 : y < -.5f ? 1 : 0;
    const auto move = [this, dx, dy] {
        if (dy != 0) {
            Navigate(dy > 0);
        } else if (dx != 0) {
            Adjust(dx);
        }
    };
    if (dx != stick_x || dy != stick_y) {
        stick_x = dx;
        stick_y = dy;
        if (dx != 0 || dy != 0) {
            move();
            stick_repeat = time + std::chrono::milliseconds(350);
        }
    } else if ((dx != 0 || dy != 0) && time >= stick_repeat) {
        move();
        stick_repeat = time + std::chrono::milliseconds(110);
    }
    if (std::any_of(Choose.begin(), Choose.end(), pressed)) {
        Activate();
    }
    if (pressed(ButtonLeftShoulder)) {
        SwitchTab(-1);
    }
    if (pressed(ButtonRightShoulder)) {
        SwitchTab(1);
    }
    if (pressed(ButtonStart) && play->isEnabled()) {
        emit PlayRequested();
    }
}

void Launcher::Navigate(bool forward) {
    QWidget* focus = QApplication::focusWidget();
    if (!focus || !isAncestorOf(focus)) {
        (play->isEnabled() ? play : browse)->setFocus(Qt::TabFocusReason);
        return;
    }
    QKeyEvent key(QEvent::KeyPress, forward ? Qt::Key_Tab : Qt::Key_Backtab,
                  forward ? Qt::NoModifier : Qt::ShiftModifier);
    QCoreApplication::sendEvent(focus, &key);
}

void Launcher::Adjust(int direction) {
    QWidget* focus = QApplication::focusWidget();
    if (!focus || !isAncestorOf(focus)) {
        Navigate(direction > 0);
    } else if (auto* slider = qobject_cast<QSlider*>(focus)) {
        slider->setValue(slider->value() + direction * slider->singleStep());
    } else if (auto* combo = qobject_cast<QComboBox*>(focus)) {
        combo->setCurrentIndex(std::clamp(combo->currentIndex() + direction, 0, combo->count() - 1));
    } else if (qobject_cast<QTabBar*>(focus)) {
        SwitchTab(direction);
    } else if (qobject_cast<QPushButton*>(focus)) {
        // Side-by-side buttons: Import / Open saves, Gamepad / Keyboard layout, pack Up / Down,
        // Quit / Play.
        Navigate(direction > 0);
    }
}

void Launcher::Activate() {
    QWidget* focus = QApplication::focusWidget();
    if (!focus || !isAncestorOf(focus)) {
        return;
    }
    if (auto* button = qobject_cast<QAbstractButton*>(focus)) {
        button->click();
    } else if (auto* combo = qobject_cast<QComboBox*>(focus)) {
        combo->setCurrentIndex((combo->currentIndex() + 1) % combo->count());
    } else if (qobject_cast<QTabBar*>(focus)) {
        Navigate(true);
    }
}

void Launcher::SwitchTab(int direction) {
    StopBind();
    RefreshControls();
    tabs->setCurrentIndex((tabs->currentIndex() + direction + tabs->count()) % tabs->count());
    tabs->tabBar()->setFocus(Qt::TabFocusReason);
}

void Launcher::FollowFocus() {
    QWidget* focus = QApplication::focusWidget();
    if (!isVisible() || !focus || !isAncestorOf(focus) || !focus->isVisible()) {
        focus_ring->hide();
        return;
    }
    QRect rect(focus->mapTo(this, QPoint(0, 0)), focus->size());
    if (auto* bar = qobject_cast<QTabBar*>(focus)) {
        const QRect tab = bar->tabRect(bar->currentIndex());
        rect = QRect(bar->mapTo(this, tab.topLeft()), tab.size());
    } else if (bindings->isAncestorOf(focus)) {
        // Scrolled bindings: only the visible part.
        rect &= QRect(bindings->viewport()->mapTo(this, QPoint(0, 0)), bindings->viewport()->size());
    } else if (mods_area->isAncestorOf(focus)) {
        rect &= QRect(mods_area->viewport()->mapTo(this, QPoint(0, 0)),
                      mods_area->viewport()->size());
    } else if (packs->isAncestorOf(focus)) {
        rect &= QRect(packs->viewport()->mapTo(this, QPoint(0, 0)), packs->viewport()->size());
    }
    const QRect ring = rect.adjusted(-4, -4, 4, 4);
    if (focus_ring->isVisible() && focus_ring->geometry() == ring) {
        return;
    }
    focus_ring->setGeometry(ring);
    focus_ring->raise();
    focus_ring->show();
    focus_ring->update();
}

bool Launcher::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::WindowActivate && watched == window() && rando_page->isVisible()) {
        // Back from Randovania's window: presets may have been made there.
        RefreshRandomizer();
    }
    if (event->type() != QEvent::KeyPress || !isVisible()) {
        return false;
    }
    auto* key = static_cast<QKeyEvent*>(event);
    if (bind_id >= 0) {
        // Rebinding: the next key binds the control (buttons only); Escape cancels.
        if (!key->isAutoRepeat()) {
            if (key->key() == Qt::Key_Escape) {
                StopBind();
                RefreshControls();
            } else if (bind_id < Controls::AnalogBase) {
                FinishBind(InputCommon::GenerateKeyboardParam(key->key()));
            }
        }
        return true;
    }
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget || (widget != this && !isAncestorOf(widget))) {
        return false;
    }
    // Arrow keys work like the D-pad.
    switch (key->key()) {
    case Qt::Key_Up:
        Navigate(false);
        return true;
    case Qt::Key_Down:
        Navigate(true);
        return true;
    case Qt::Key_Left:
        Adjust(-1);
        return true;
    case Qt::Key_Right:
        Adjust(1);
        return true;
    default:
        return false;
    }
}

void Launcher::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    previous.fill(true); // ignore buttons already held when the launcher appears
    poll_timer->start(16);
    QTimer::singleShot(0, this, [this] { FollowFocus(); });
}

void Launcher::hideEvent(QHideEvent* event) {
    poll_timer->stop();
    StopBind();
    focus_ring->hide();
    QWidget::hideEvent(event);
}

void Launcher::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    QTimer::singleShot(0, this, [this] { FollowFocus(); });
}

} // namespace MSR
