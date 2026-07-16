#pragma once

#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QVector>
#include <QQueue>
#include <QString>
#include <atomic>
#include <cstdint>

extern "C" {
#include <ecrt.h>
}

/* CiA402 操作模式 */
enum OpMode : int8_t {
    MODE_PP  = 1,   // Profile Position
    MODE_PV  = 3,   // Profile Velocity
    MODE_PT  = 4,   // Profile Torque
    MODE_HM  = 6,   // Homing
    MODE_IP  = 7,   // Interpolated Position
    MODE_CSP = 8,   // Cyclic Sync Position
    MODE_CSV = 9,   // Cyclic Sync Velocity
    MODE_CST = 10   // Cyclic Sync Torque
};

struct MotorStatus {
    uint16_t statusWord = 0;
    int32_t  actualPos  = 0;
    int32_t  actualVel  = 0;
    int16_t  actualTor  = 0;
    int8_t   opModeDisp = 0;
    uint16_t errorCode  = 0;
    uint8_t  alState    = 0;
    bool     online     = false;
    bool     isMotor    = false;   // 是否匹配 cfg.vendor/product 的电机从站
    uint32_t vendorId   = 0;       // 总线读到的实际 vendor
    uint32_t productCode= 0;       // 总线读到的实际 product
    uint32_t revisionNumber=0;     // EEPROM 中的固件/ESI 修订号（143 或 145）
};

struct MotorCommand {
    int8_t  opMode     = MODE_CSP;
    int32_t targetPos  = 0;
    int32_t targetVel  = 0;
    int16_t targetTor  = 0;
    bool    enable     = false;   // 使能请求
    bool    faultReset = false;   // 故障复位脉冲（消费后自动清零）
    bool    absolute   = true;    // CSP 位置含义：true=相对起始位置偏移(推荐)，false=点动累加
                                  // PP  位置含义：true=绝对(CW bit6=0)，false=相对(CW bit6=1)
    int32_t jogStep    = 0;       // CSP 每周期最大步长(脉冲)，同时用作"点动"时的增量
    bool    hasTarget  = false;   // 用户是否已显式"应用目标值"。false=保持使能瞬间的位置
    uint32_t applySeq  = 0;       // 每次"应用目标值"自增，worker 据此为 PP/PV/HM 触发 new_setpoint 脉冲
};

struct SdoJob {
    int      slave    = 0;
    uint16_t index    = 0;
    uint8_t  subindex = 0;
    int      bits     = 16;       // 8 / 16 / 32
    bool     isSigned = false;
    bool     write    = false;
    int64_t  value    = 0;
    QString  tag;                 // 标识返回
};

struct SdoResult {
    QString  tag;
    bool     ok = false;
    int64_t  value = 0;
    QString  err;
};

struct EcConfig {
    int      slaveCount = 2;
    uint32_t vendor     = 0x00001097;   // 电机识别用 vendor
    uint32_t product    = 0x00002406;   // 电机识别用 product
    int      cycleUs    = 1000;         // 1ms
    /* 可选：每个总线位置是否为电机的预设掩码（长度应等于 slaveCount）。
       留空时 worker 会按实际总线扫描到的 vendor/product 自动判定。 */
    QVector<bool> motorMask;
};

struct RecordedMotion {
    int sampleMs = 20;
    QVector<QVector<int32_t>> frames; // 每帧与总线位置等长，非电机位置为 0
};
Q_DECLARE_METATYPE(RecordedMotion)

class EcWorker : public QThread {
    Q_OBJECT
public:
    explicit EcWorker(QObject *parent = nullptr);
    ~EcWorker() override;

    void configure(const EcConfig &cfg);
    void requestStop();

    /* SDO 调参模式：true=暂停 PDO 控制(所有电机下电)，仅维持通讯；false=恢复正常控制 */
    void setSdoSafeMode(bool on) { sdoSafe_ = on; }
    bool sdoSafeMode() const     { return sdoSafe_; }

    /* 由 GUI 调用（线程安全） */
    void setCommand(int slave, const MotorCommand &cmd);
    QVector<MotorStatus> snapshot();
    void postSdo(const SdoJob &job);
    bool beginMotionRecord(int sampleMs);
    void endMotionRecord();
    bool beginMotionReturnToStart(const RecordedMotion &motion, int returnSpeed,
                                  bool keepEnabled = false);
    bool startPreparedMotionPlayback();
    void stopMotionActivity();

signals:
    void logMessage(QString msg);
    void masterStarted();
    void masterStopped();
    void errorOccurred(QString msg);
    void sdoFinished(SdoResult res);
    void motionRecordFinished(RecordedMotion motion);
    void motionStateChanged(QString state);

protected:
    void run() override;

private:
    bool initMaster();
    void cleanup();
    void doOneSdo(const SdoJob &job);

    /* SDO 子线程：阻塞的 ecrt_master_sdo_* 调用不能放在 RT 循环里 */
    class SdoThread;
    friend class SdoThread;
    SdoThread      *sdoThread_ = nullptr;
    QMutex          sdoMtx_;
    QWaitCondition  sdoCv_;
    QQueue<SdoJob>  sdoQueue_;
    std::atomic<bool> sdoRun_{false};
    std::atomic<bool> sdoBusy_{false}; // 当前是否阻塞在 ecrt_master_sdo_* 调用中

    EcConfig cfg_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> sdoSafe_{false};

    ec_master_t *master_ = nullptr;
    ec_domain_t *domain_ = nullptr;
    QVector<ec_slave_config_t*> sc_;
    uint8_t *domain_pd_ = nullptr;

    /* PDO 偏移 */
    QVector<uint32_t> off_cw_, off_tp_, off_tv_, off_tt_, off_om_, off_r1_;
    QVector<uint32_t> off_sw_, off_ap_, off_av_, off_at_, off_omd_, off_err_, off_r2_;

    /* 共享数据 */
    QMutex mtx_;
    QVector<MotorCommand> cmds_;
    QVector<MotorStatus>  status_;
    QVector<bool>         isMotor_;    // 与 slaveCount 等长；非电机位置跳过 PDO 控制
    QVector<bool>         enabled_;
    QVector<int32_t>      startPos_;   // 进入 OperationEnabled 瞬间的实际位置（做基准，避免跳变）
    QVector<int32_t>      curTp_;      // CSP 每周期实际下发的目标位置（用于梯形限速逼近 targetPos）
    QVector<uint32_t>     lastSeq_;    // 上次已处理的 applySeq，用于检测"新一次应用"
    QVector<uint8_t>      ppPhase_;    // PP/PV/HM new_setpoint 脉冲状态机：0=idle,1=置位等待 ack,2=清位等待 ack 回落

    enum MotionMode { MotionIdle, MotionRecording, MotionReturning, MotionReady, MotionPlaying };
    QMutex motionMtx_;
    MotionMode motionMode_ = MotionIdle;
    RecordedMotion recorded_, playback_;
    int recordTick_ = 0;
    int returnSpeed_ = 100000;          // encoder counts / s
    QVector<int32_t> motionTarget_;
    int playTick_ = 0;
};
