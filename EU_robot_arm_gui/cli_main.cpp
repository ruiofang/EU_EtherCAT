#include "ecworker.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTextStream>
#include <QTextCodec>
#include <QTimer>

#include <cstdio>

extern "C" {
#include <ecrt.h>
}

struct CliMotion { QString name, created; RecordedMotion data; };

class CliController : public QObject {
public:
    explicit CliController(QObject *parent = nullptr) : QObject(parent), input_(fileno(stdin), QSocketNotifier::Read, this) {
        connect(&input_, &QSocketNotifier::activated, this, [this] { readCommand(); });
        /* CLI 默认静默运行：不转发周期诊断等普通日志，避免刷屏和终端 I/O 抢占。 */
        connect(&worker_, &EcWorker::errorOccurred, this, [this](const QString &s) { out("[错误] " + s); });
        connect(&worker_, &EcWorker::masterStarted, this, [this] { running_ = true; out("主站已启动"); showMenu(); });
        connect(&worker_, &EcWorker::masterStopped, this, [this] { running_ = false; out("主站已停止"); });
        connect(&worker_, &EcWorker::motionStateChanged, this, [this](const QString &s) {
            motionState_ = s; out("[动作] " + s);
            if (s == "已到起点" && autoPlay_) { autoPlay_ = false; worker_.startPreparedMotionPlayback(); }
            if (s == "播放完成") QTimer::singleShot(100, this, [this] { writeBrakes(false); });
        });
        connect(&worker_, &EcWorker::motionRecordFinished, this, [this](const RecordedMotion &m) { saveRecorded(m); });
        connect(&worker_, &EcWorker::sdoFinished, this, [this](const SdoResult &r) {
            if (!r.ok) out(QString("[SDO失败] %1 %2").arg(r.tag, r.err));
        });
        loadMotions();
        QTimer::singleShot(0, this, [this] { startMaster(); });
    }

    ~CliController() override { worker_.requestStop(); worker_.wait(); }

private:
    void out(const QString &s) {
        QTextStream ts(stdout); ts.setCodec("UTF-8");
        ts << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz ") << s << Qt::endl;
    }
    void prompt(const QString &s = "请选择> ") {
        QTextStream ts(stdout); ts.setCodec("UTF-8"); ts << s << Qt::flush;
    }

    void showMenu() {
        QTextStream ts(stdout); ts.setCodec("UTF-8");
        ts << QStringLiteral(
               "\n========== EU 机械臂 CLI ==========\n"
               " 1. 查看电机状态\n"
               " 2. 查看动作列表\n"
               " 3. 全部使能\n"
               " 4. 一键故障复位\n"
               " 5. 全部失能并松开抱闸\n"
               " 6. 全部失能并抱闸\n"
               " 7. 开始录制\n"
               " 8. 停止录制并命名保存\n"
               " 9. 回到动作起点\n"
               "10. 播放（需先到起点）\n"
               "11. 回起点并播放\n"
               "12. 停止当前动作\n"
               "13. 设置采样周期/回起点速度\n"
               " 0. 退出\n")
           << QString("当前设置：采样=%1 ms，回起点速度=%2 脉冲/s\n").arg(recordMs_).arg(returnSpeed_)
           << QStringLiteral("====================================\n");
        ts.flush(); prompt();
    }

    void showMotionChoices() {
        if (motions_.isEmpty()) { out("没有已保存动作"); inputMode_ = MenuInput; showMenu(); return; }
        QTextStream ts(stdout); ts.setCodec("UTF-8"); ts << QStringLiteral("\n请选择动作：\n");
        for (int i = 0; i < motions_.size(); ++i)
            ts << QString(" %1. %2（%3帧，%4ms/帧）\n").arg(i + 1).arg(motions_[i].name)
                  .arg(motions_[i].data.frames.size()).arg(motions_[i].data.sampleMs);
        ts.flush(); prompt("动作编号（0取消）> ");
    }

    QString libraryPath() const {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dir); return dir + "/robot_motions.json";
    }

    void loadMotions() {
        motions_.clear(); QFile f(libraryPath());
        if (!f.open(QIODevice::ReadOnly)) return;
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        for (const auto &v : doc.object().value("motions").toArray()) {
            QJsonObject o = v.toObject(); CliMotion m;
            m.name = o.value("name").toString(); m.created = o.value("created").toString();
            m.data.sampleMs = o.value("sample_ms").toInt(20);
            int axes = o.value("axes").toInt();
            for (const auto &fv : o.value("frames").toArray()) {
                QVector<int32_t> frame;
                for (const auto &p : fv.toArray()) frame.push_back((int32_t)p.toDouble());
                if (frame.size() == axes) m.data.frames.push_back(frame);
            }
            if (!m.name.isEmpty() && m.data.frames.size() >= 2) motions_.push_back(m);
        }
        out(QString("已加载 %1 条动作：%2").arg(motions_.size()).arg(libraryPath()));
    }

    bool saveLibrary() {
        QJsonArray list;
        for (const auto &m : motions_) {
            QJsonObject o; o["name"] = m.name; o["created"] = m.created; o["sample_ms"] = m.data.sampleMs;
            o["axes"] = m.data.frames.isEmpty() ? 0 : m.data.frames.first().size();
            QJsonArray frames;
            for (const auto &f : m.data.frames) { QJsonArray a; for (int32_t p : f) a.append((double)p); frames.append(a); }
            o["frames"] = frames; list.append(o);
        }
        QJsonObject root; root["version"] = 1; root["motions"] = list;
        QSaveFile f(libraryPath());
        return f.open(QIODevice::WriteOnly) && f.write(QJsonDocument(root).toJson()) >= 0 && f.commit();
    }

    const CliMotion *findMotion(const QString &name) const {
        for (const auto &m : motions_) if (m.name == name) return &m;
        return nullptr;
    }

    void startMaster() {
        ec_master_t *m = ecrt_request_master(0);
        if (!m) {
            if (++scanAttempts_ < 20) { QTimer::singleShot(500, this, [this]{ startMaster(); }); return; }
            out("无法占用 EtherCAT Master 0，请确认没有其他程序正在使用"); showMenu(); return;
        }
        ec_master_info_t mi{}; ecrt_master(m, &mi);
        EcConfig cfg; cfg.slaveCount = (int)mi.slave_count; cfg.cycleUs = 1000;
        cfg.motorMask.fill(false, cfg.slaveCount);
        for (int i = 0; i < cfg.slaveCount; ++i) {
            ec_slave_info_t si{}; ecrt_master_get_slave(m, (uint16_t)i, &si);
            if (si.vendor_id == cfg.vendor && si.product_code == cfg.product) cfg.motorMask[i] = true;
        }
        ecrt_release_master(m);
        if (cfg.slaveCount <= 0) {
            if (++scanAttempts_ < 20) { QTimer::singleShot(500, this, [this]{ startMaster(); }); return; }
            out("等待 10 秒后仍未扫描到从站，请检查 EtherCAT 服务、网线和从站供电"); showMenu(); return;
        }
        scanAttempts_ = 0;
        out(QString("扫描到 %1 个从站，正在启动主站...").arg(cfg.slaveCount));
        worker_.configure(cfg); worker_.start(QThread::TimeCriticalPriority);
    }

    void setEnableAll(bool enable) {
        auto s = worker_.snapshot();
        for (int i = 0; i < s.size(); ++i) if (s[i].isMotor) {
            MotorCommand c; c.enable = enable; c.opMode = MODE_CSP; c.hasTarget = false;
            worker_.setCommand(i, c);
        }
    }

    void resetFaults() {
        auto s = worker_.snapshot(); int count = 0;
        for (int i = 0; i < s.size(); ++i) if (s[i].isMotor) {
            MotorCommand c; c.opMode = MODE_CSP; c.enable = false;
            c.hasTarget = false; c.faultReset = true;
            worker_.setCommand(i, c); ++count;
        }
        out(QString("已向 %1 个 EU 电机发送 Controlword 0x0080 故障复位脉冲").arg(count));
    }

    void writeBrakes(bool open) {
        auto s = worker_.snapshot();
        for (int i = 0; i < s.size(); ++i) if (s[i].isMotor) {
            SdoJob j; j.slave = i; j.index = 0x2014; j.subindex = 1; j.bits = 8;
            j.write = true; j.value = open ? 1 : 0; j.tag = QString("CLI-BRAKE-%1-%2").arg(i).arg(open);
            worker_.postSdo(j);
        }
        out(open ? "已请求松开全部抱闸" : "已请求抱死全部抱闸");
    }

    void returnMotion(const QString &name, int speed, bool autoPlay) {
        const CliMotion *m = findMotion(name);
        if (!m) { out("动作不存在：" + name); return; }
        autoPlay_ = autoPlay; setEnableAll(false); writeBrakes(true);
        RecordedMotion motion = m->data;
        QTimer::singleShot(150, this, [this, motion, speed] {
            if (!worker_.beginMotionReturnToStart(motion, speed)) out("无法启动回起点任务");
        });
    }

    void saveRecorded(const RecordedMotion &motion) {
        writeBrakes(false);
        if (pendingRecordName_.isEmpty()) { out("录制已停止但没有名称，未保存"); return; }
        int found = -1; for (int i = 0; i < motions_.size(); ++i) if (motions_[i].name == pendingRecordName_) found = i;
        CliMotion m{pendingRecordName_, QDateTime::currentDateTime().toString(Qt::ISODate), motion};
        if (found >= 0) motions_[found] = m; else motions_.push_back(m);
        out(saveLibrary() ? QString("动作 %1 已保存，共 %2 帧").arg(m.name).arg(motion.frames.size()) : "动作库保存失败");
        pendingRecordName_.clear();
    }

    void readCommand() {
        QTextStream in(stdin); in.setCodec("UTF-8"); const QString raw = in.readLine();
        if (raw.isNull()) { input_.setEnabled(false); return; }
        const QString line = raw.trimmed();
        if (inputMode_ == RecordNameInput) {
            if (line.isEmpty()) { prompt("动作名称不能为空，请重新输入> "); return; }
            pendingRecordName_ = line; inputMode_ = MenuInput; worker_.endMotionRecord(); showMenu(); return;
        }
        if (inputMode_ == ReturnMotionInput || inputMode_ == RunMotionInput) {
            bool ok = false; int index = line.toInt(&ok) - 1;
            if (line == "0") { inputMode_ = MenuInput; showMenu(); return; }
            if (!ok || index < 0 || index >= motions_.size()) { prompt("动作编号无效，请重新输入（0取消）> "); return; }
            bool run = inputMode_ == RunMotionInput; inputMode_ = MenuInput;
            returnMotion(motions_[index].name, returnSpeed_, run); showMenu(); return;
        }
        if (inputMode_ == SettingsInput) {
            QStringList v = line.split(' ', Qt::SkipEmptyParts); bool ok1=false, ok2=false;
            int ms = v.value(0).toInt(&ok1), speed = v.value(1).toInt(&ok2);
            if (!ok1 || !ok2 || ms < 5 || ms > 1000 || speed <= 0) {
                prompt("格式错误，请输入：采样周期ms 回起点速度（例如 20 50000）> "); return;
            }
            recordMs_ = ms; returnSpeed_ = speed; inputMode_ = MenuInput;
            out(QString("设置完成：采样=%1 ms，回起点速度=%2 脉冲/s").arg(recordMs_).arg(returnSpeed_));
            showMenu(); return;
        }

        bool ok = false; int choice = line.toInt(&ok);
        if (!ok) { out("请输入菜单中的数字"); showMenu(); return; }
        if (choice == 1) {
            out(QString("主站=%1 动作状态=%2").arg(running_ ? "运行" : "停止", motionState_));
            auto s = worker_.snapshot(); for (int i=0;i<s.size();++i) if(s[i].isMotor)
                out(QString("#%1 AL=0x%2 SW=0x%3 POS=%4 ERR=0x%5").arg(i).arg(s[i].alState,2,16,QChar('0')).arg(s[i].statusWord,4,16,QChar('0')).arg(s[i].actualPos).arg(s[i].errorCode,4,16,QChar('0')));
        } else if (choice == 2) {
            for (const auto &m : motions_) out(QString("%1  %2帧  %3ms/帧").arg(m.name).arg(m.data.frames.size()).arg(m.data.sampleMs));
        } else if (choice == 3) setEnableAll(true);
        else if (choice == 4) resetFaults();
        else if (choice == 5) { worker_.stopMotionActivity(); setEnableAll(false); QTimer::singleShot(100, this, [this]{ writeBrakes(true); }); }
        else if (choice == 6) { setEnableAll(false); QTimer::singleShot(100, this, [this]{ writeBrakes(false); }); }
        else if (choice == 7) {
            setEnableAll(false); writeBrakes(true);
            QTimer::singleShot(150, this, [this]{ if (!worker_.beginMotionRecord(recordMs_)) out("无法开始录制"); });
        } else if (choice == 8) { inputMode_ = RecordNameInput; prompt("请输入动作名称> "); return;
        } else if (choice == 9) { inputMode_ = ReturnMotionInput; showMotionChoices(); return;
        } else if (choice == 10) { if (!worker_.startPreparedMotionPlayback()) out("请先选择 9 回起点，并等待状态显示已到起点"); }
        else if (choice == 11) { inputMode_ = RunMotionInput; showMotionChoices(); return;
        } else if (choice == 12) { autoPlay_ = false; worker_.stopMotionActivity(); QTimer::singleShot(100, this, [this]{ writeBrakes(false); }); }
        else if (choice == 13) { inputMode_ = SettingsInput; prompt("输入：采样周期ms 回起点速度（例如 20 50000）> "); return;
        } else if (choice == 0) { worker_.requestStop(); worker_.wait(); QCoreApplication::quit(); return; }
        else out("菜单编号无效");
        showMenu();
    }

    EcWorker worker_;
    QSocketNotifier input_;
    QVector<CliMotion> motions_;
    bool running_ = false, autoPlay_ = false;
    QString motionState_ = "空闲", pendingRecordName_;
    enum InputMode { MenuInput, RecordNameInput, ReturnMotionInput, RunMotionInput, SettingsInput };
    InputMode inputMode_ = MenuInput;
    int recordMs_ = 20;
    int returnSpeed_ = 100000;
    int scanAttempts_ = 0;
};

int main(int argc, char **argv) {
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("");
    QCoreApplication::setApplicationName("EU_robot_arm_gui"); // 与 GUI 共用动作库目录
    qRegisterMetaType<RecordedMotion>("RecordedMotion");
    CliController controller;
    return app.exec();
}
