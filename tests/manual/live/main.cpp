// Copyright (C) 2024 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "helper.h"

#include <WServer>
#include <wsocket.h>
#include <WXdgShell>
#include <WOutput>
#include <WSeat>
#include <WBackend>
#include <wquickcursor.h>
#include <wquickoutputlayout.h>
#include <wrenderhelper.h>
#include <woutputrenderwindow.h>
#include <woutputviewport.h>
#include <WXdgSurface>
#include <wqmlcreator_p.h>

#include <qwbackend.h>
#include <qwdisplay.h>
#include <qwoutput.h>
#include <qwlogging.h>
#include <qwcompositor.h>
#include <qwsubcompositor.h>
#include <qwcompositor.h>
#include <qwrenderer.h>
#include <qwallocator.h>

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QProcess>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>

QW_USE_NAMESPACE

Helper::Helper(QObject *parent)
    : QObject(parent)
    , m_server(new WServer(this))
    , m_outputCreator(new WQmlCreator(this))
    , m_xdgShellCreator(new WQmlCreator(this))
    , m_outputLayout(new WQuickOutputLayout(this))
    , m_cursor(new WQuickCursor(this))
{
    m_seat = m_server->attach<WSeat>();
    m_seat->setCursor(m_cursor);
    m_cursor->setThemeName(getenv("XCURSOR_THEME"));
    m_cursor->setLayout(m_outputLayout);
}

void Helper::initProtocols(WOutputRenderWindow *window, QQmlEngine *qmlEngine)
{
    m_backend = m_server->attach<WBackend>();
    m_server->start();

    m_renderer = WRenderHelper::createRenderer(m_backend->handle());

    if (!m_renderer) {
        qFatal("Failed to create renderer");
    }

    m_socket = new WSocket(false);
    if (m_socket->autoCreate()) {
        m_server->addSocket(m_socket);
    } else {
        delete m_socket;
        qCritical("Failed to create socket");
    }

    connect(m_backend, &WBackend::outputAdded, this, [this, window, qmlEngine] (WOutput *output) {
        auto initProperties = qmlEngine->newObject();
        initProperties.setProperty("waylandOutput", qmlEngine->toScriptValue(output));
        initProperties.setProperty("waylandCursor", qmlEngine->toScriptValue(m_cursor));
        initProperties.setProperty("layout", qmlEngine->toScriptValue(m_outputLayout));
        initProperties.setProperty("x", qmlEngine->toScriptValue(m_outputLayout->implicitWidth()));

        m_outputCreator->add(output, initProperties);
    });

    connect(m_backend, &WBackend::outputRemoved, this, [this] (WOutput *output) {
        m_outputCreator->removeByOwner(output);
    });

    connect(m_backend, &WBackend::inputAdded, this, [this] (WInputDevice *device) {
        m_seat->attachInputDevice(device);
    });

    connect(m_backend, &WBackend::inputRemoved, this, [this] (WInputDevice *device) {
        m_seat->detachInputDevice(device);
    });

    m_allocator = QWAllocator::autoCreate(m_backend->handle(), m_renderer);
    m_renderer->initWlDisplay(m_server->handle());

    // free follow display
    m_compositor = QWCompositor::create(m_server->handle(), m_renderer, 6);
    QWSubcompositor::create(m_server->handle());

    connect(window, &WOutputRenderWindow::outputViewportInitialized, this, [] (WOutputViewport *viewport) {
        // Trigger QWOutput::frame signal in order to ensure WOutputHelper::renderable
        // property is true, OutputRenderWindow when will render this output in next frame.
        {
            WOutput *output = viewport->output();

            // Enable on default
            auto qwoutput = output->handle();
            // Don't care for WOutput::isEnabled, must do WOutput::commit here,
            // In order to ensure trigger QWOutput::frame signal, WOutputRenderWindow
            // needs this signal to render next frmae. Because QWOutput::frame signal
            // maybe emit before WOutputRenderWindow::attach, if no commit here,
            // WOutputRenderWindow will ignore this ouptut on render.
            if (!qwoutput->property("_Enabled").toBool()) {
                qwoutput->setProperty("_Enabled", true);

                if (!qwoutput->handle()->current_mode) {
                    auto mode = qwoutput->preferredMode();
                    if (mode)
                        output->setMode(mode);
                }
                output->enable(true);
                bool ok = output->commit();
                Q_ASSERT(ok);
            }
        }
    });
    window->init(m_renderer, m_allocator);

    auto *xdgShell = m_server->attach<WXdgShell>();

    connect(xdgShell, &WXdgShell::surfaceAdded, this, [this, qmlEngine](WXdgSurface *surface) {
        auto initProperties = qmlEngine->newObject();
        initProperties.setProperty("type", surface->isPopup() ? "popup" : "toplevel");
        initProperties.setProperty("waylandSurface", qmlEngine->toScriptValue(surface));
        m_xdgShellCreator->add(surface, initProperties);

    });
    connect(xdgShell, &WXdgShell::surfaceRemoved, m_xdgShellCreator, &WQmlCreator::removeByOwner);

    m_backend->handle()->start();
    QProcess waylandClientDemo;

    waylandClientDemo.setProgram(PROJECT_BINARY_DIR"/examples/animationclient/animationclient");
    waylandClientDemo.setArguments({"-platform", "wayland"});
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("WAYLAND_DISPLAY", m_socket->fullServerName());

    waylandClientDemo.setProcessEnvironment(env);
    waylandClientDemo.startDetached();
}

int main(int argc, char *argv[]) {
    QWLog::init();
    WServer::initializeQPA();
    QQuickStyle::setStyle("Material");

    QGuiApplication::setAttribute(Qt::AA_UseOpenGLES);
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QGuiApplication::setQuitOnLastWindowClosed(false);
    QGuiApplication app(argc, argv);

    QQmlApplicationEngine waylandEngine;
    waylandEngine.loadFromModule("Live", "Main");

    auto window = waylandEngine.rootObjects().first()->findChild<WOutputRenderWindow*>();
    Q_ASSERT(window);

    Helper *helper = waylandEngine.singletonInstance<Helper*>("Live", "Helper");
    Q_ASSERT(helper);

    helper->initProtocols(window, &waylandEngine);

    return app.exec();
}