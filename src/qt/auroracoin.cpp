// Copyright (c) 2011-2018 The DigiByte Core developers
// Copyright (c) 2014-2019 The Auroracoin Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/auroracoin-config.h>
#endif

#include <qt/auroracoin.h>
#include <qt/auroracoingui.h>

#include <chainparams.h>
#include <qt/clientmodel.h>
#include <fs.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/intro.h>
#include <qt/networkstyle.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/splashscreen.h>
#include <qt/utilitydialog.h>
#include <qt/winshutdownmonitor.h>

#ifdef ENABLE_WALLET
#include <qt/paymentserver.h>
#include <qt/walletmodel.h>
#endif

#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <noui.h>
#include <rpc/server.h>
#include <ui_interface.h>
#include <uint256.h>
#include <util/system.h>
#include <warnings.h>

#include <walletinitinterface.h>

#include <memory>
#include <stdint.h>

#include <boost/thread.hpp>

#include <QApplication>
#include <QDebug>
#include <QLibraryInfo>
#include <QLocale>
#include <QMessageBox>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QTranslator>

#if defined(QT_STATICPLUGIN)
#include <QtPlugin>
#if QT_VERSION < 0x050400
Q_IMPORT_PLUGIN(AccessibleFactory)
#endif
#if defined(QT_QPA_PLATFORM_XCB)
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_WINDOWS)
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_COCOA)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin);
#endif
#endif

// Declare meta types used for QMetaObject::invokeMethod
Q_DECLARE_METATYPE(bool*)
Q_DECLARE_METATYPE(CAmount)
Q_DECLARE_METATYPE(uint256)

static QString GetLangTerritory()
{
    QSettings settings;
    // Get desired locale (e.g. "de_DE")
    // 1) System default language
    QString lang_territory = QLocale::system().name();
    // 2) Language from QSettings
    QString lang_territory_qsettings = settings.value("language", "").toString();
    if(!lang_territory_qsettings.isEmpty())
        lang_territory = lang_territory_qsettings;
    // 3) -lang command line argument
    lang_territory = QString::fromStdString(gArgs.GetArg("-lang", lang_territory.toStdString()));
    return lang_territory;
}

/** Set up translations */
static void initTranslations(QTranslator &qtTranslatorBase, QTranslator &qtTranslator, QTranslator &translatorBase, QTranslator &translator)
{
    // Remove old translators
    QApplication::removeTranslator(&qtTranslatorBase);
    QApplication::removeTranslator(&qtTranslator);
    QApplication::removeTranslator(&translatorBase);
    QApplication::removeTranslator(&translator);

    // Get desired locale (e.g. "de_DE")
    // 1) System default language
    QString lang_territory = GetLangTerritory();

    // Convert to "de" only by truncating "_DE"
    QString lang = lang_territory;
    lang.truncate(lang_territory.lastIndexOf('_'));

    // Load language files for configured locale:
    // - First load the translator for the base language, without territory
    // - Then load the more specific locale translator

    // Load e.g. qt_de.qm
    if (qtTranslatorBase.load("qt_" + lang, QLibraryInfo::location(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslatorBase);

    // Load e.g. qt_de_DE.qm
    if (qtTranslator.load("qt_" + lang_territory, QLibraryInfo::location(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslator);

    // Load e.g. auroracoin_de.qm (shortcut "de" needs to be defined in auroracoin.qrc)
    if (translatorBase.load(lang, ":/translations/"))
        QApplication::installTranslator(&translatorBase);

    // Load e.g. auroracoin_de_DE.qm (shortcut "de_DE" needs to be defined in auroracoin.qrc)
    if (translator.load(lang_territory, ":/translations/"))
        QApplication::installTranslator(&translator);
}

/* qDebug() message handler --> debug.log */
void DebugMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString &msg)
{
    Q_UNUSED(context);
    if (type == QtDebugMsg) {
        LogPrint(BCLog::QT, "GUI: %s\n", msg.toStdString());
    } else {
        LogPrintf("GUI: %s\n", msg.toStdString());
    }
}

AuroracoinCore::AuroracoinCore(interfaces::Node& node) :
    QObject(), m_node(node)
{
}

void AuroracoinCore::handleRunawayException(const std::exception *e)
{
    PrintExceptionContinue(e, "Runaway exception");
    Q_EMIT runawayException(QString::fromStdString(m_node.getWarnings("gui")));
}

void AuroracoinCore::initialize()
{
    try
    {
        qDebug() << __func__ << ": Running initialization in thread";
        bool rv = m_node.appInitMain();
        Q_EMIT initializeResult(rv);
    } catch (const std::exception& e) {
        handleRunawayException(&e);
    } catch (...) {
        handleRunawayException(nullptr);
    }
}

void AuroracoinCore::shutdown()
{
    try
    {
        qDebug() << __func__ << ": Running Shutdown in thread";
        m_node.appShutdown();
        qDebug() << __func__ << ": Shutdown finished";
        Q_EMIT shutdownResult();
    } catch (const std::exception& e) {
        handleRunawayException(&e);
    } catch (...) {
        handleRunawayException(nullptr);
    }
}

AuroracoinApplication::AuroracoinApplication(interfaces::Node& node, int &argc, char **argv):
    QApplication(argc, argv),
    coreThread(0),
    m_node(node),
    optionsModel(0),
    clientModel(0),
    window(0),
    pollShutdownTimer(0),
#ifdef ENABLE_WALLET
    paymentServer(0),
    m_wallet_models(),
#endif
    returnValue(0),
    platformStyle(0)
{
    setQuitOnLastWindowClosed(false);
}

void AuroracoinApplication::setupPlatformStyle()
{
    // UI per-platform customization
    // This must be done inside the AuroracoinApplication constructor, or after it, because
    // PlatformStyle::instantiate requires a QApplication
    std::string platformName;
    platformName = gArgs.GetArg("-uiplatform", AuroracoinGUI::DEFAULT_UIPLATFORM);
    platformStyle = PlatformStyle::instantiate(QString::fromStdString(platformName));
    if (!platformStyle) // Fall back to "other" if specified name not found
        platformStyle = PlatformStyle::instantiate("other");
    assert(platformStyle);
}

AuroracoinApplication::~AuroracoinApplication()
{
    if(coreThread)
    {
        qDebug() << __func__ << ": Stopping thread";
        Q_EMIT stopThread();
        coreThread->wait();
        qDebug() << __func__ << ": Stopped thread";
    }

    delete window;
    window = 0;
#ifdef ENABLE_WALLET
    delete paymentServer;
    paymentServer = 0;
#endif
    delete optionsModel;
    optionsModel = 0;
    delete platformStyle;
    platformStyle = 0;
}

#ifdef ENABLE_WALLET
void AuroracoinApplication::createPaymentServer()
{
    paymentServer = new PaymentServer(this);
}
#endif

void AuroracoinApplication::createOptionsModel(bool resetSettings)
{
    optionsModel = new OptionsModel(m_node, nullptr, resetSettings);
}

void AuroracoinApplication::createWindow(const NetworkStyle *networkStyle)
{
    window = new AuroracoinGUI(m_node, platformStyle, networkStyle, 0);

    pollShutdownTimer = new QTimer(window);
    connect(pollShutdownTimer, &QTimer::timeout, window, &AuroracoinGUI::detectShutdown);
}

void AuroracoinApplication::createSplashScreen(const NetworkStyle *networkStyle)
{
    SplashScreen *splash = new SplashScreen(m_node, 0, networkStyle);
    // We don't hold a direct pointer to the splash screen after creation, but the splash
    // screen will take care of deleting itself when finish() happens.
    splash->show();
    connect(this, &AuroracoinApplication::splashFinished, splash, &SplashScreen::finish);
    connect(this, &AuroracoinApplication::requestedShutdown, splash, &QWidget::close);
}

bool AuroracoinApplication::baseInitialize()
{
    return m_node.baseInitialize();
}

void AuroracoinApplication::startThread()
{
    if(coreThread)
        return;
    coreThread = new QThread(this);
    AuroracoinCore *executor = new AuroracoinCore(m_node);
    executor->moveToThread(coreThread);

    /*  communication to and from thread */
    connect(executor, &AuroracoinCore::initializeResult, this, &AuroracoinApplication::initializeResult);
    connect(executor, &AuroracoinCore::shutdownResult, this, &AuroracoinApplication::shutdownResult);
    connect(executor, &AuroracoinCore::runawayException, this, &AuroracoinApplication::handleRunawayException);
    connect(this, &AuroracoinApplication::requestedInitialize, executor, &AuroracoinCore::initialize);
    connect(this, &AuroracoinApplication::requestedShutdown, executor, &AuroracoinCore::shutdown);
    /*  make sure executor object is deleted in its own thread */
    connect(this, &AuroracoinApplication::stopThread, executor, &QObject::deleteLater);
    connect(this, &AuroracoinApplication::stopThread, coreThread, &QThread::quit);

    coreThread->start();
}

void AuroracoinApplication::parameterSetup()
{
    // Default printtoconsole to false for the GUI. GUI programs should not
    // print to the console unnecessarily.
    gArgs.SoftSetBoolArg("-printtoconsole", false);

    m_node.initLogging();
    m_node.initParameterInteraction();
}

void AuroracoinApplication::requestInitialize()
{
    qDebug() << __func__ << ": Requesting initialize";
    startThread();
    Q_EMIT requestedInitialize();
}

void AuroracoinApplication::requestShutdown()
{
    // Show a simple window indicating shutdown status
    // Do this first as some of the steps may take some time below,
    // for example the RPC console may still be executing a command.
    shutdownWindow.reset(ShutdownWindow::showShutdownWindow(window));

    qDebug() << __func__ << ": Requesting shutdown";
    startThread();
    window->hide();
    window->setClientModel(0);
    pollShutdownTimer->stop();

#ifdef ENABLE_WALLET
    window->removeAllWallets();
    for (const WalletModel* walletModel : m_wallet_models) {
        delete walletModel;
    }
    m_wallet_models.clear();
#endif
    delete clientModel;
    clientModel = 0;

    m_node.startShutdown();

    // Request shutdown from core thread
    Q_EMIT requestedShutdown();
}

void AuroracoinApplication::addWallet(WalletModel* walletModel)
{
#ifdef ENABLE_WALLET
    window->addWallet(walletModel);

    if (m_wallet_models.empty()) {
        window->setCurrentWallet(walletModel->getWalletName());
    }

#ifdef ENABLE_BIP70
    connect(walletModel, &WalletModel::coinsSent,
        paymentServer, &PaymentServer::fetchPaymentACK);
#endif
    connect(walletModel, &WalletModel::unload, this, &AuroracoinApplication::removeWallet);

    m_wallet_models.push_back(walletModel);
#endif
}

void AuroracoinApplication::removeWallet()
{
#ifdef ENABLE_WALLET
    WalletModel* walletModel = static_cast<WalletModel*>(sender());
    m_wallet_models.erase(std::find(m_wallet_models.begin(), m_wallet_models.end(), walletModel));
    window->removeWallet(walletModel);
    walletModel->deleteLater();
#endif
}

void AuroracoinApplication::initializeResult(bool success)
{
    qDebug() << __func__ << ": Initialization result: " << success;
    // Set exit result.
    returnValue = success ? EXIT_SUCCESS : EXIT_FAILURE;
    if(success)
    {
        // Log this only after AppInitMain finishes, as then logging setup is guaranteed complete
        qWarning() << "Platform customization:" << platformStyle->getName();
#ifdef ENABLE_WALLET
#ifdef ENABLE_BIP70
        PaymentServer::LoadRootCAs();
#endif
        if (paymentServer) paymentServer->setOptionsModel(optionsModel);
#endif

        clientModel = new ClientModel(m_node, optionsModel);
        window->setClientModel(clientModel);

#ifdef ENABLE_WALLET
        m_handler_load_wallet = m_node.handleLoadWallet([this](std::unique_ptr<interfaces::Wallet> wallet) {
            WalletModel* wallet_model = new WalletModel(std::move(wallet), m_node, platformStyle, optionsModel, nullptr);
            // Fix wallet model thread affinity.
            wallet_model->moveToThread(thread());
            QMetaObject::invokeMethod(this, "addWallet", Qt::QueuedConnection, Q_ARG(WalletModel*, wallet_model));
        });

        for (auto& wallet : m_node.getWallets()) {
            addWallet(new WalletModel(std::move(wallet), m_node, platformStyle, optionsModel));
        }
#endif

        // If -min option passed, start window minimized (iconified) or minimized to tray
        if (!gArgs.GetBoolArg("-min", false)) {
            window->show();
        } else if (clientModel->getOptionsModel()->getMinimizeToTray() && window->hasTrayIcon()) {
            // do nothing as the window is managed by the tray icon
        } else {
            window->showMinimized();
        }
        Q_EMIT splashFinished();
        Q_EMIT windowShown(window);

#ifdef ENABLE_WALLET
        // Now that initialization/startup is done, process any command-line
        // auroracoin: URIs or payment requests:
        if (paymentServer) {
            connect(paymentServer, &PaymentServer::receivedPaymentRequest, window, &AuroracoinGUI::handlePaymentRequest);
            connect(window, &AuroracoinGUI::receivedURI, paymentServer, &PaymentServer::handleURIOrFile);
            connect(paymentServer, &PaymentServer::message, [this](const QString& title, const QString& message, unsigned int style) {
                window->message(title, message, style);
            });
            QTimer::singleShot(100, paymentServer, &PaymentServer::uiReady);
        }
#endif
        pollShutdownTimer->start(200);
    } else {
        Q_EMIT splashFinished(); // Make sure splash screen doesn't stick around during shutdown
        quit(); // Exit first main loop invocation
    }
}

void AuroracoinApplication::shutdownResult()
{
    quit(); // Exit second main loop invocation after shutdown finished
}

void AuroracoinApplication::handleRunawayException(const QString &message)
{
    QMessageBox::critical(0, "Runaway exception", AuroracoinGUI::tr("A fatal error occurred. Auroracoin can no longer continue safely and will quit.") + QString("\n\n") + message);
    ::exit(EXIT_FAILURE);
}

WId AuroracoinApplication::getMainWinId() const
{
    if (!window)
        return 0;

    return window->winId();
}

static void SetupUIArgs()
{
#if defined(ENABLE_WALLET) && defined(ENABLE_BIP70)
    gArgs.AddArg("-allowselfsignedrootcertificates", strprintf("Allow self signed root certificates (default: %u)", DEFAULT_SELFSIGNED_ROOTCERTS), true, OptionsCategory::GUI);
#endif
    gArgs.AddArg("-choosedatadir", strprintf("Choose data directory on startup (default: %u)", DEFAULT_CHOOSE_DATADIR), false, OptionsCategory::GUI);
    gArgs.AddArg("-lang=<lang>", "Set language, for example \"de_DE\" (default: system locale)", false, OptionsCategory::GUI);
    gArgs.AddArg("-min", "Start minimized", false, OptionsCategory::GUI);
    gArgs.AddArg("-resetguisettings", "Reset all settings changed in the GUI", false, OptionsCategory::GUI);
    gArgs.AddArg("-rootcertificates=<file>", "Set SSL root certificates for payment request (default: -system-)", false, OptionsCategory::GUI);
    gArgs.AddArg("-splash", strprintf("Show splash screen on startup (default: %u)", DEFAULT_SPLASHSCREEN), false, OptionsCategory::GUI);
    gArgs.AddArg("-uiplatform", strprintf("Select platform to customize UI for (one of windows, macosx, other; default: %s)", AuroracoinGUI::DEFAULT_UIPLATFORM), true, OptionsCategory::GUI);
}

#ifndef AURORACOIN_QT_TEST
int GuiMain(int argc, char* argv[])
{
#ifdef WIN32
    util::WinCmdLineArgs winArgs;
    std::tie(argc, argv) = winArgs.get();
#endif
    SetupEnvironment();

    std::unique_ptr<interfaces::Node> node = interfaces::MakeNode();

    // Subscribe to global signals from core
    std::unique_ptr<interfaces::Handler> handler_message_box = node->handleMessageBox(noui_ThreadSafeMessageBox);
    std::unique_ptr<interfaces::Handler> handler_question = node->handleQuestion(noui_ThreadSafeQuestion);
    std::unique_ptr<interfaces::Handler> handler_init_message = node->handleInitMessage(noui_InitMessage);

    // Do not refer to data directory yet, this can be overridden by Intro::pickDataDirectory

    /// 1. Basic Qt initialization (not dependent on parameters or configuration)
    Q_INIT_RESOURCE(auroracoin);
    Q_INIT_RESOURCE(auroracoin_locale);

    AuroracoinApplication app(*node, argc, argv);
    // Generate high-dpi pixmaps
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#if QT_VERSION >= 0x050600
    QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
#ifdef Q_OS_MAC
    QApplication::setAttribute(Qt::AA_DontShowIconsInMenus);
#endif

    // Register meta types used for QMetaObject::invokeMethod
    qRegisterMetaType< bool* >();
    //   Need to pass name here as CAmount is a typedef (see http://qt-project.org/doc/qt-5/qmetatype.html#qRegisterMetaType)
    //   IMPORTANT if it is no longer a typedef use the normal variant above
    qRegisterMetaType< CAmount >("CAmount");
    qRegisterMetaType< std::function<void()> >("std::function<void()>");
#ifdef ENABLE_WALLET
    qRegisterMetaType<WalletModel*>("WalletModel*");
#endif

    /// 2. Parse command-line options. We do this after qt in order to show an error if there are problems parsing these
    // Command-line options take precedence:
    node->setupServerArgs();
    SetupUIArgs();
    std::string error;
    if (!node->parseParameters(argc, argv, error)) {
        QMessageBox::critical(0, QObject::tr(PACKAGE_NAME),
            QObject::tr("Error parsing command line arguments: %1.").arg(QString::fromStdString(error)));
        return EXIT_FAILURE;
    }

    // Now that the QApplication is setup and we have parsed our parameters, we can set the platform style
    app.setupPlatformStyle();

    /// 3. Application identification
    // must be set before OptionsModel is initialized or translations are loaded,
    // as it is used to locate QSettings
    QApplication::setOrganizationName(QAPP_ORG_NAME);
    QApplication::setOrganizationDomain(QAPP_ORG_DOMAIN);
    QApplication::setApplicationName(QAPP_APP_NAME_DEFAULT);

    /// 4. Initialization of translations, so that intro dialog is in user's language
    // Now that QSettings are accessible, initialize translations
    QTranslator qtTranslatorBase, qtTranslator, translatorBase, translator;
    initTranslations(qtTranslatorBase, qtTranslator, translatorBase, translator);

    // Show help message immediately after parsing command-line options (for "-lang") and setting locale,
    // but before showing splash screen.
    if (HelpRequested(gArgs) || gArgs.IsArgSet("-version")) {
        HelpMessageDialog help(*node, nullptr, gArgs.IsArgSet("-version"));
        help.showOrPrint();
        return EXIT_SUCCESS;
    }

    /// 5. Now that settings and translations are available, ask user for data directory
    // User language is set up: pick a data directory
    if (!Intro::pickDataDirectory(*node))
        return EXIT_SUCCESS;

    /// 6. Determine availability of data and blocks directory and parse auroracoin.conf
    /// - Do not call GetDataDir(true) before this step finishes
    if (!fs::is_directory(GetDataDir(false)))
    {
        QMessageBox::critical(0, QObject::tr(PACKAGE_NAME),
                              QObject::tr("Error: Specified data directory \"%1\" does not exist.").arg(QString::fromStdString(gArgs.GetArg("-datadir", ""))));
        return EXIT_FAILURE;
    }
    if (!node->readConfigFiles(error)) {
        QMessageBox::critical(0, QObject::tr(PACKAGE_NAME),
            QObject::tr("Error: Cannot parse configuration file: %1.").arg(QString::fromStdString(error)));
        return EXIT_FAILURE;
    }

    /// 7. Determine network (and switch to network specific options)
    // - Do not call Params() before this step
    // - Do this after parsing the configuration file, as the network can be switched there
    // - QSettings() will use the new application name after this, resulting in network-specific settings
    // - Needs to be done before createOptionsModel

    // Check for -testnet or -regtest parameter (Params() calls are only valid after this clause)
    try {
        node->selectParams(gArgs.GetChainName());
    } catch(std::exception &e) {
        QMessageBox::critical(0, QObject::tr(PACKAGE_NAME), QObject::tr("Error: %1").arg(e.what()));
        return EXIT_FAILURE;
    }
#ifdef ENABLE_WALLET
    // Parse URIs on command line -- this can affect Params()
    PaymentServer::ipcParseCommandLine(*node, argc, argv);
#endif

    QScopedPointer<const NetworkStyle> networkStyle(NetworkStyle::instantiate(QString::fromStdString(Params().NetworkIDString())));
    assert(!networkStyle.isNull());
    // Allow for separate UI settings for testnets
    QApplication::setApplicationName(networkStyle->getAppName());
    // Re-initialize translations after changing application name (language in network-specific settings can be different)
    initTranslations(qtTranslatorBase, qtTranslator, translatorBase, translator);

#ifdef ENABLE_WALLET
    /// 8. URI IPC sending
    // - Do this early as we don't want to bother initializing if we are just calling IPC
    // - Do this *after* setting up the data directory, as the data directory hash is used in the name
    // of the server.
    // - Do this after creating app and setting up translations, so errors are
    // translated properly.
    if (PaymentServer::ipcSendCommandLine())
        exit(EXIT_SUCCESS);

    // Start up the payment server early, too, so impatient users that click on
    // auroracoin: links repeatedly have their payment requests routed to this process:
    app.createPaymentServer();
#endif

    /// 9. Main GUI initialization
    // Install global event filter that makes sure that long tooltips can be word-wrapped
    app.installEventFilter(new GUIUtil::ToolTipToRichTextFilter(TOOLTIP_WRAP_THRESHOLD, &app));
#if defined(Q_OS_WIN)
    // Install global event filter for processing Windows session related Windows messages (WM_QUERYENDSESSION and WM_ENDSESSION)
    qApp->installNativeEventFilter(new WinShutdownMonitor());
#endif
    // Install qDebug() message handler to route to debug.log
    qInstallMessageHandler(DebugMessageHandler);
    // Allow parameter interaction before we create the options model
    app.parameterSetup();
    // Load GUI settings from QSettings
    app.createOptionsModel(gArgs.GetBoolArg("-resetguisettings", false));

    if (gArgs.GetBoolArg("-splash", DEFAULT_SPLASHSCREEN) && !gArgs.GetBoolArg("-min", false))
        app.createSplashScreen(networkStyle.data());

    int rv = EXIT_SUCCESS;
    try
    {
        app.createWindow(networkStyle.data());
        // Perform base initialization before spinning up initialization/shutdown thread
        // This is acceptable because this function only contains steps that are quick to execute,
        // so the GUI thread won't be held up.
        if (app.baseInitialize()) {
            app.requestInitialize();
#if defined(Q_OS_WIN)
            WinShutdownMonitor::registerShutdownBlockReason(QObject::tr("%1 didn't yet exit safely...").arg(QObject::tr(PACKAGE_NAME)), (HWND)app.getMainWinId());
#endif
            app.exec();
            app.requestShutdown();
            app.exec();
            rv = app.getReturnValue();
        } else {
            // A dialog with detailed error will have been shown by InitError()
            rv = EXIT_FAILURE;
        }
    } catch (const std::exception& e) {
        PrintExceptionContinue(&e, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(node->getWarnings("gui")));
    } catch (...) {
        PrintExceptionContinue(nullptr, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(node->getWarnings("gui")));
    }
    return rv;
}
#endif // AURORACOIN_QT_TEST
