#include <QApplication>
#include <QObject>
#include <QMetaType>
#include <csignal>
#include "mainwindow.h"
#include "ecworker.h"

static QApplication *g_app = nullptr;

static void sigHandler(int) {
    /* 让 Qt 事件循环退出，触发 MainWindow 析构 -> EcWorker 停止 -> master 释放 */
    if (g_app) QMetaObject::invokeMethod(g_app, "quit", Qt::QueuedConnection);
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    g_app = &app;

    /* 跨线程信号参数必须注册，否则队列连接会丢弃并打印 "Cannot queue arguments" */
    qRegisterMetaType<SdoResult>("SdoResult");
    qRegisterMetaType<MotorStatus>("MotorStatus");

    std::signal(SIGINT,  sigHandler);
    std::signal(SIGTERM, sigHandler);
    std::signal(SIGHUP,  sigHandler);

    MainWindow w;
    w.show();
    return app.exec();
}
