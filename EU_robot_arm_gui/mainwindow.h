#pragma once

#include <QMainWindow>
#include <QVector>
#include "ecworker.h"

class QSpinBox;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QTimer;
class QTabWidget;
class QCheckBox;
class QListWidget;
class QFile;

/* 每个电机 Tab 所用控件集合 */
struct MotorPanel {
    QLabel      *lblAl;
    QLabel      *lblSw;
    QLabel      *lblPos, *lblVel, *lblTor, *lblErr, *lblModeDisp;
    QComboBox   *cbMode;
    QSpinBox    *spTarget;        // 目标位置 / 速度 / 力矩（根据模式）
    QSpinBox    *spJogStep;
    QSpinBox    *spProfVel;       // 轮廓速度 0x6081 (PP 模式下使用)
    QSpinBox    *spProfAcc;       // 轮廓加速 0x6083
    QSpinBox    *spProfDec;       // 轮廓减速 0x6084
    QSpinBox    *spTorSlope;      // 力矩斜率 0x6087 (PT)
    QSpinBox    *spHomeMethod;    // 回零方法 0x6098 (HM)
    QSpinBox    *spHomeSpdSw;     // 回零搜索开关速度 0x6099:01 (HM)
    QSpinBox    *spHomeSpdZero;   // 回零搜索零点速度 0x6099:02 (HM)
    QSpinBox    *spHomeAcc;       // 回零加速度 0x609A (HM)
    QSpinBox    *spHomeOffset;    // 原点偏移 0x607C (HM)
    QSpinBox    *spIpTimeMs;      // 插补周期毫秒 0x60C2:01 (IP)
    QCheckBox   *chkAbs;
    QPushButton *btnEnable, *btnDisable, *btnFaultReset, *btnApply;
    /* 抱闸 (0x2014:01 写 / 0x2014:02 读) */
    QPushButton *btnBrakeOpen, *btnBrakeClose, *btnBrakeQuery;
    QLabel      *lblBrakeState;
    /* SDO */
    QLineEdit   *edIdx, *edSub, *edVal;
    QComboBox   *cbBits, *cbSign;
    QPushButton *btnSdoRead, *btnSdoWrite;
    QLabel      *lblSdoResult;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onStart();
    void onStop();
    void onRefreshStatus();
    void onMasterStarted();
    void onMasterStopped();
    void onLog(const QString &m);
    void onError(const QString &m);
    void onSdoResult(SdoResult r);

    void onApply(int motor);
    void onEnable(int motor);
    void onDisable(int motor);
    void onFaultReset(int motor);
    void onSdoRead(int motor);
    void onSdoWrite(int motor);
    void onModeChanged(int motor);
    void onToggleSdoSafe(bool on);
    void onScan();

    void onBrakeOpen(int motor);
    void onBrakeClose(int motor);
    void onBrakeQuery(int motor);
    void onRecordToggle();
    void onEnableAllMotion();
    void onFaultResetAllMotion();
    void onDisableReleaseAll();
    void onReturnMotion();
    void onReturnAndPlayMotion();
    void onPlayMotion();
    void onStopMotion();
    void onDeleteMotion();
    void onMotionRecorded(RecordedMotion motion);
    void onMotionState(const QString &state);

private:
    void buildUi();
    void rebuildMotorTabs(int n);
    void appendLog(const QString &m);
    void setLogSaving(bool enabled);
    void loadMotionLibrary();
    bool saveMotionLibrary();
    void refreshMotionList();
    void setAllMotorCommandsDisabled();
    void writeAllBrakes(bool open, const QString &tagPrefix);

    /* 顶部配置 */
    QSpinBox    *spSlaves_;
    QLineEdit   *edVendor_, *edProduct_;
    QSpinBox    *spCycleUs_;
    QPushButton *btnStart_, *btnStop_;
    QLabel      *lblMasterState_;
    QPushButton *btnSdoSafe_;
    QPushButton *btnScan_;
    QCheckBox   *chkSaveLog_;

    QTabWidget     *tabs_;
    QPlainTextEdit *log_;
    QTimer         *timer_;
    QFile          *logFile_ = nullptr;

    EcWorker            *worker_ = nullptr;
    QVector<MotorPanel>  panels_;
    QVector<bool>        motorMask_;   // 扫描后每个总线位置是否为电机
    bool                 running_ = false;

    QListWidget  *motionList_;
    QSpinBox     *spRecordMs_, *spReturnSpeed_;
    QPushButton  *btnRecord_, *btnEnableAll_, *btnFaultResetAll_, *btnDisableRelease_, *btnReturn_;
    QPushButton  *btnPlay_, *btnReturnPlay_, *btnMotionStop_, *btnMotionDelete_;
    QLabel       *lblMotionState_;
    struct SavedMotion { QString name, created; RecordedMotion data; };
    QVector<SavedMotion> motions_;
    bool recording_ = false;
    bool recordPreparing_ = false;
    int pendingRecordBrakes_ = 0;
    int failedRecordBrakes_ = 0;
    int pendingRecordSampleMs_ = 20;
    bool playbackPreparing_ = false;
    int pendingPlaybackBrakes_ = 0;
    int failedPlaybackBrakes_ = 0;
    int pendingPlaybackSpeed_ = 100000;
    RecordedMotion pendingPlaybackMotion_;
    QString pendingPlaybackName_;
    bool playbackActive_ = false;
    bool autoPlayAfterReturn_ = false;
};
