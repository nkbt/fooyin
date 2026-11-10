// SPDX-License-Identifier: GPL-3.0-or-later
#include "toolbarmanager.h"

#include <gui/guisettings.h>
#include <gui/theme/themeregistry.h>
#include <gui/widgetprovider.h>
#include <utils/actions/actionmanager.h>
#include <utils/settings/settingsmanager.h>

#include <QApplication>
#include <QImage>
#include <QMainWindow>
#include <QMenuBar>
#include <QPainter>
#include <QStyleFactory>
#include <QStyleOptionMenuItem>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>

#include <memory>

using namespace Qt::StringLiterals;

namespace Fooyin::Testing {
namespace {
class PaletteObserver : public QObject
{
public:
    int paletteChanges{0};
    bool eventFilter(QObject*, QEvent* event) override
    {
        if(event->type() == QEvent::PaletteChange) {
            ++paletteChanges;
        }
        return false;
    }
};

QPalette themePalette(const FyTheme& theme, const QPalette& systemPalette)
{
    auto palette = systemPalette;
    for(auto it = theme.colours.cbegin(); it != theme.colours.cend(); ++it) {
        palette.setColor(it.key().group, it.key().role, it.value());
    }
    return palette;
}

void publishTheme(SettingsManager& settings, const FyTheme& theme, const QPalette& palette, quint64 revision)
{
    settings.set<Settings::Gui::CustomTheme>(QVariant::fromValue(theme));
    QApplication::setPalette(palette);
    ResolvedAppStyle resolved;
    resolved.palette  = palette;
    resolved.revision = revision;
    settings.set<Settings::Gui::ResolvedAppStyle>(QVariant::fromValue(resolved));
    QTest::qWait(100);
}

void compareGroup(const QPalette& actual, const QPalette& expected, QPalette::ColorGroup group)
{
    QCOMPARE(actual.color(group, QPalette::Window), expected.color(group, QPalette::Window));
    QCOMPARE(actual.color(group, QPalette::WindowText), expected.color(group, QPalette::WindowText));
    QCOMPARE(actual.color(group, QPalette::Text), expected.color(group, QPalette::Text));
    QCOMPARE(actual.color(group, QPalette::ButtonText), expected.color(group, QPalette::ButtonText));
    QCOMPARE(actual.color(group, QPalette::Highlight), expected.color(group, QPalette::Highlight));
    QCOMPARE(actual.color(group, QPalette::HighlightedText), expected.color(group, QPalette::HighlightedText));
}

void comparePalette(const QPalette& actual, const QPalette& expected)
{
    compareGroup(actual, expected, QPalette::Active);
    compareGroup(actual, expected, QPalette::Inactive);
    compareGroup(actual, expected, QPalette::Disabled);
}

QImage renderLabel(QMenuBar* menu, const QPalette& palette, bool selected)
{
    QImage image{QSize{140, 40}, QImage::Format_ARGB32_Premultiplied};
    image.fill(palette.color(QPalette::Window));
    QStyleOptionMenuItem option;
    option.initFrom(menu);
    option.rect    = image.rect();
    option.text    = u"File"_s;
    option.palette = palette;
    option.palette.setCurrentColorGroup(QPalette::Active);
    option.state = QStyle::State_Enabled | QStyle::State_Active;
    if(selected) {
        option.state |= QStyle::State_Selected | QStyle::State_Sunken;
    }
    QPainter painter{&image};
    menu->style()->drawControl(QStyle::CE_MenuBarItem, &option, &painter, menu);
    return image;
}

void compareRendering(QMenuBar* menu, const QPalette& expected)
{
    const auto normal   = renderLabel(menu, menu->palette(), false);
    const auto selected = renderLabel(menu, menu->palette(), true);
    QCOMPARE(normal, renderLabel(menu, expected, false));
    QCOMPARE(selected, renderLabel(menu, expected, true));
    QVERIFY(normal != selected);
    const QString path = qEnvironmentVariable("FOOYIN_MENU_PALETTE_EVIDENCE");
    if(!path.isEmpty()) {
        QVERIFY(normal.save(path + u"/normal.png"_s));
        QVERIFY(selected.save(path + u"/selected.png"_s));
    }
}
} // namespace

class MenuPaletteTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void customColorsSurviveBreezeHeaderAndPlacement()
    {
        auto* breeze = QStyleFactory::create(u"Breeze"_s);
        if(!breeze) {
            if(QStyleFactory::keys().contains(u"Breeze"_s, Qt::CaseInsensitive)) {
                QFAIL("Advertised Breeze renderer failed to load");
            }
            QSKIP("Optional Breeze renderer is not installed");
        }
        QApplication::setStyle(breeze);
        QApplication::setPalette(QPalette{});
        const QPalette systemPalette = QApplication::palette();
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        SettingsManager settings{directory.filePath(u"settings.ini"_s)};
        settings.createTempSetting<Settings::Gui::LayoutEditing>(false);
        settings.createTempSetting<Settings::Gui::MenuBarMovable>(true);
        settings.createTempSetting<Settings::Gui::ToolbarsLocked>(true);
        settings.createTempSetting<Settings::Gui::CustomTheme>(QVariant{});
        settings.createTempSetting<Settings::Gui::ResolvedAppStyle>(QVariant::fromValue(ResolvedAppStyle{}));
        ThemeRegistry themes{&settings};
        FyTheme nord;
        FyTheme classic;
        for(const auto& theme : themes.items()) {
            if(theme.name == u"Nord"_s) {
                nord = theme;
            }
            if(theme.name == u"Classic Dark"_s) {
                classic = theme;
            }
        }
        QVERIFY(!nord.colours.isEmpty());
        QVERIFY(!classic.colours.isEmpty());
        const auto nordPalette    = themePalette(nord, systemPalette);
        const auto classicPalette = themePalette(classic, systemPalette);
        QMainWindow window;
        ActionManager actions{&settings};
        actions.setMainWindow(&window);
        WidgetProvider widgets;
        auto* menu = new QMenuBar;
        const QPointer<QMenuBar> identity{menu};
        menu->addAction(u"File"_s);
        menu->addAction(u"Help"_s);
        auto toolbars = std::make_unique<ToolbarManager>(&window, menu, &actions, &widgets, &settings, nullptr);
        window.resize(600, 220);
        window.show();
        QTest::qWait(100);
        auto* toolbar = window.findChild<QToolBar*>(u"MenuToolbar"_s);
        QVERIFY(toolbar);
        QVERIFY(menu->parentWidget() == toolbar);
        publishTheme(settings, nord, nordPalette, 1);
        QVERIFY(toolbar->property("breeze_has_toolsarea_palette").toBool());
        QVERIFY(toolbar->palette().color(QPalette::WindowText) != nordPalette.color(QPalette::WindowText));
        comparePalette(menu->palette(), nordPalette);
        compareRendering(menu, nordPalette);
        publishTheme(settings, classic, classicPalette, 2);
        comparePalette(menu->palette(), classicPalette);
        publishTheme(settings, nord, nordPalette, 3);
        QApplication::setStyle(QStyleFactory::create(u"Fusion"_s));
        QTest::qWait(100);
        comparePalette(menu->palette(), nordPalette);
        QApplication::setStyle(QStyleFactory::create(u"Breeze"_s));
        QTest::qWait(100);
        comparePalette(menu->palette(), nordPalette);
        settings.set<Settings::Gui::MenuBarMovable>(false);
        QTest::qWait(100);
        QCOMPARE(window.menuWidget(), menu);
        QCOMPARE(identity.data(), menu);
        comparePalette(menu->palette(), nordPalette);
        PaletteObserver observer;
        menu->installEventFilter(&observer);
        menu->style()->unpolish(&window);
        menu->style()->polish(&window);
        menu->style()->unpolish(&window);
        menu->style()->polish(&window);
        QTest::qWait(100);
        const int settledPaletteChanges = observer.paletteChanges;
        QTest::qWait(150);
        QCOMPARE(observer.paletteChanges, settledPaletteChanges);
        comparePalette(menu->palette(), nordPalette);
        compareRendering(menu, nordPalette);
        QMainWindow nativeWindow;
        auto* nativeMenu = new QMenuBar;
        nativeMenu->addAction(u"File"_s);
        nativeWindow.setMenuBar(nativeMenu);
        auto* nativeToolbar = new QToolBar{&nativeWindow};
        nativeWindow.addToolBar(nativeToolbar);
        nativeWindow.show();
        publishTheme(settings, FyTheme{}, systemPalette, 4);
        comparePalette(menu->palette(), nativeMenu->palette());
        FyTheme fontOnly;
        fontOnly.fonts.insert(QString{}, QApplication::font());
        publishTheme(settings, fontOnly, systemPalette, 5);
        comparePalette(menu->palette(), nativeMenu->palette());
        publishTheme(settings, nord, nordPalette, 6);
        menu->style()->unpolish(&window);
        menu->style()->polish(&window);
        const auto freshNativePalette = menu->palette();
        QVERIFY(freshNativePalette.color(QPalette::WindowText) != nordPalette.color(QPalette::WindowText));
        settings.set<Settings::Gui::CustomTheme>(QVariant::fromValue(fontOnly));
        ResolvedAppStyle fontOnlyStyle;
        fontOnlyStyle.palette  = systemPalette;
        fontOnlyStyle.revision = 7;
        settings.set<Settings::Gui::ResolvedAppStyle>(QVariant::fromValue(fontOnlyStyle));
        QTest::qWait(100);
        comparePalette(menu->palette(), freshNativePalette);
        publishTheme(settings, fontOnly, systemPalette, 8);
        comparePalette(menu->palette(), nativeMenu->palette());
        settings.set<Settings::Gui::MenuBarMovable>(true);
        nativeToolbar->addWidget(nativeMenu);
        QTest::qWait(100);
        QCOMPARE(menu->parentWidget(), toolbar);
        QCOMPARE(identity.data(), menu);
        publishTheme(settings, nord, nordPalette, 9);
        comparePalette(menu->palette(), nordPalette);
        publishTheme(settings, FyTheme{}, systemPalette, 10);
        comparePalette(menu->palette(), nativeMenu->palette());
        publishTheme(settings, fontOnly, systemPalette, 11);
        comparePalette(menu->palette(), nativeMenu->palette());
        QCOMPARE(QApplication::style()->metaObject()->className(), "Breeze::Style");
        QCOMPARE(window.findChildren<QToolBar*>().size(), 1);
        publishTheme(settings, nord, nordPalette, 12);
        menu->style()->unpolish(&window);
        menu->style()->polish(&window);
        toolbars.reset();
        QTest::qWait(100);
        QCOMPARE(identity.data(), menu);
        QCOMPARE(menu->parentWidget(), toolbar);
    }
};
} // namespace Fooyin::Testing

int main(int argc, char** argv)
{
    QTemporaryDir profile;
    if(!profile.isValid()) {
        return 2;
    }
    qputenv("HOME", profile.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", profile.path().toUtf8());
    qputenv("XDG_CONFIG_DIRS", profile.path().toUtf8());
    qputenv("XDG_DATA_HOME", profile.path().toUtf8());
    qputenv("XDG_CACHE_HOME", profile.path().toUtf8());
    qputenv("XDG_STATE_HOME", profile.path().toUtf8());
    qputenv("DBUS_SESSION_BUS_ADDRESS", (u"unix:path="_s + profile.path() + u"/absent"_s).toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_PLATFORMTHEME", "");
    qputenv("QT_STYLE_OVERRIDE", "Fusion");
    QApplication application{argc, argv};
    Fooyin::Testing::MenuPaletteTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "menupalettetest.moc"
