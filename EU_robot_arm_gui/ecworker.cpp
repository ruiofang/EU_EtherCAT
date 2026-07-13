#include "ecworker.h"

#include <QMutexLocker>
#include <QDateTime>
#include <QElapsedTimer>
#include <unistd.h>
#include <time.h>
#include <cstring>
#include <climits>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

/* ============ PDO 描述（与 main.cpp 示例保持一致） ============ */
static ec_pdo_entry_info_t g_pdo_entries[] = {
    /* RxPDO 0x1600 */
    {0x6040, 0x00, 16},  // Control Word
    {0x607A, 0x00, 32},  // Target Position
    {0x60FF, 0x00, 32},  // Target Velocity
    {0x6071, 0x00, 16},  // Target Torque
    {0x6060, 0x00, 8},   // Mode of Operation
    {0x60C2, 0x01, 8},   // 占位（16 字节对齐）

    /* TxPDO 0x1A00 */
    {0x6041, 0x00, 16},  // Status Word
    {0x6064, 0x00, 32},  // Actual Position
    {0x606C, 0x00, 32},  // Actual Velocity
    {0x6077, 0x00, 16},  // Actual Torque
    {0x6061, 0x00, 8},   // Mode of Operation Display
    {0x603F, 0x00, 16},  // Error Code
    {0x2026, 0x00, 8},   // 占位
};

static ec_pdo_info_t g_rx_pdo[] = {
    {0x1600, 6, g_pdo_entries},
};
static ec_pdo_info_t g_tx_pdo[] = {
    {0x1A00, 7, &g_pdo_entries[6]},
};
static ec_sync_info_t g_syncs[] = {
    {0, EC_DIR_OUTPUT, 0, nullptr,  EC_WD_DISABLE},
    {1, EC_DIR_INPUT,  0, nullptr,  EC_WD_DISABLE},
    {2, EC_DIR_OUTPUT, 1, g_rx_pdo, EC_WD_ENABLE},
    {3, EC_DIR_INPUT,  1, g_tx_pdo, EC_WD_DISABLE},
    {0xff}
};

/* IgH ecrt 不会在运行时解析 ESI XML；此绑定用来根据 EEPROM
   RevisionNo 选择与之匹配的 ESI 定义。两个版本当前使用的
   0x1600/0x1A00 PDO 布局相同，由上面的 g_syncs 下发。 */
static const char *xmlForRevision(uint32_t revision) {
    switch (revision) {
    case 143: return "EYOU_ServoModule_ECAT_V143.xml";
    case 145: return "EYOU_ServoModule_ECAT_V145.xml";
    default:  return nullptr;
    }
}

/* ================================================================ */
EcWorker::EcWorker(QObject *parent) : QThread(parent) {}

EcWorker::~EcWorker() {
    requestStop();
    wait();
}

void EcWorker::configure(const EcConfig &cfg) { cfg_ = cfg; }
void EcWorker::requestStop()                  { stopRequested_ = true; }

void EcWorker::setCommand(int slave, const MotorCommand &cmd) {
    QMutexLocker lk(&mtx_);
    if (slave >= 0 && slave < cmds_.size()) {
        /* faultReset 是一次性脉冲 -> 若新请求为 true 则置位，否则保留旧值直至 worker 消费 */
        bool keepReset = cmds_[slave].faultReset && !cmd.faultReset;
        cmds_[slave] = cmd;
        if (keepReset) cmds_[slave].faultReset = true;
    }
}

QVector<MotorStatus> EcWorker::snapshot() {
    QMutexLocker lk(&mtx_);
    return status_;
}

void EcWorker::postSdo(const SdoJob &job) {
    QMutexLocker lk(&sdoMtx_);
    sdoQueue_.enqueue(job);
    sdoCv_.wakeOne();
}

bool EcWorker::beginMotionRecord(int sampleMs) {
    if (!running_ || sampleMs < 5 || sampleMs > 1000) return false;
    {
        QMutexLocker ml(&motionMtx_);
        if (motionMode_ != MotionIdle) return false;
        { QMutexLocker lk(&mtx_); for (MotorCommand &c : cmds_) c.enable = false; }
        recorded_ = RecordedMotion{};
        recorded_.sampleMs = sampleMs;
        recordTick_ = 0;
        motionMode_ = MotionRecording;
    }
    emit motionStateChanged("录制中（电机失能）");
    return true;
}

void EcWorker::endMotionRecord() {
    RecordedMotion out;
    {
        QMutexLocker ml(&motionMtx_);
        if (motionMode_ != MotionRecording) return;
        motionMode_ = MotionIdle;
        out = recorded_;
    }
    emit motionStateChanged("空闲");
    emit motionRecordFinished(out);
}

bool EcWorker::beginMotionReturnToStart(const RecordedMotion &motion, int returnSpeed, bool keepEnabled) {
    if (!running_ || motion.frames.size() < 2 || motion.sampleMs < 1 || returnSpeed <= 0)
        return false;
    for (const auto &f : motion.frames) if (f.size() != cfg_.slaveCount) return false;
    {
        QMutexLocker ml(&motionMtx_);
        if (motionMode_ != MotionIdle) return false;
        if (!keepEnabled) {
            QMutexLocker lk(&mtx_);
            for (MotorCommand &c : cmds_) c.enable = false;
        }
        playback_ = motion;
        returnSpeed_ = returnSpeed;
        motionTarget_.clear();
        playTick_ = 0;
        motionMode_ = MotionReturning;
    }
    emit motionStateChanged("回到动作起点");
    return true;
}

bool EcWorker::startPreparedMotionPlayback() {
    QMutexLocker ml(&motionMtx_);
    if (!running_ || motionMode_ != MotionReady || playback_.frames.size() < 2) return false;
    playTick_ = 0;
    motionMode_ = MotionPlaying;
    emit motionStateChanged("播放中");
    return true;
}

void EcWorker::stopMotionActivity() {
    RecordedMotion out; bool hadRecording = false;
    {
        QMutexLocker ml(&motionMtx_);
        if (motionMode_ == MotionRecording) { out = recorded_; hadRecording = true; }
        motionMode_ = MotionIdle;
        { QMutexLocker lk(&mtx_); for (MotorCommand &c : cmds_) c.enable = false; }
    }
    if (hadRecording) emit motionRecordFinished(out);
    emit motionStateChanged("空闲");
}

/* ================= SDO 子线程 ================= */
class EcWorker::SdoThread : public QThread {
public:
    explicit SdoThread(EcWorker *w) : w_(w) {}
protected:
    void run() override {
        while (w_->sdoRun_) {
            SdoJob job;
            {
                QMutexLocker lk(&w_->sdoMtx_);
                while (w_->sdoRun_ && w_->sdoQueue_.isEmpty())
                    w_->sdoCv_.wait(&w_->sdoMtx_, 200);
                if (!w_->sdoRun_) return;
                if (w_->sdoQueue_.isEmpty()) continue;
                job = w_->sdoQueue_.dequeue();
            }
            w_->doOneSdo(job);
        }
    }
private:
    EcWorker *w_;
};

/* ================================================================ */
bool EcWorker::initMaster() {
    const int N = cfg_.slaveCount;
    cmds_.clear();   cmds_.resize(N);
    status_.clear(); status_.resize(N);
    isMotor_.clear(); isMotor_.resize(N);
    enabled_.clear(); enabled_.resize(N);
    startPos_.clear(); startPos_.resize(N);
    curTp_.clear();  curTp_.resize(N);
    lastSeq_.clear(); lastSeq_.resize(N);
    ppPhase_.clear(); ppPhase_.resize(N);
    sc_.clear(); sc_.resize(N);   // 默认 nullptr

    off_cw_.fill(0, N);  off_tp_.fill(0, N);  off_tv_.fill(0, N);
    off_tt_.fill(0, N);  off_om_.fill(0, N);  off_r1_.fill(0, N);
    off_sw_.fill(0, N);  off_ap_.fill(0, N);  off_av_.fill(0, N);
    off_at_.fill(0, N);  off_omd_.fill(0, N); off_err_.fill(0, N); off_r2_.fill(0, N);

    master_ = ecrt_request_master(0);
    if (!master_) { emit errorOccurred("ecrt_request_master(0) 失败"); return false; }

    domain_ = ecrt_master_create_domain(master_);
    if (!domain_) { emit errorOccurred("ecrt_master_create_domain 失败"); return false; }

    /* ===== 先扫描总线，按 vendor/product 区分电机与非电机设备 ===== */
    ec_master_info_t mi{};
    if (ecrt_master(master_, &mi) != 0) {
        emit errorOccurred("ecrt_master(info) 失败");
        return false;
    }
    if ((int)mi.slave_count < N) {
        emit logMessage(QString("<警告> 配置的从站数=%1，但总线仅检测到 %2 个从站；"
                                "超出范围的位置将被跳过").arg(N).arg(mi.slave_count));
    }

    const bool useMaskHint = (cfg_.motorMask.size() == N);
    QString motorList, otherList;
    for (int i = 0; i < N; ++i) {
        MotorStatus &s = status_[i];
        s.isMotor = false;
        if (i >= (int)mi.slave_count) {
            isMotor_[i] = false;
            continue;   // 该位置总线上不存在
        }
        ec_slave_info_t si{};
        if (ecrt_master_get_slave(master_, (uint16_t)i, &si) != 0) {
            emit logMessage(QString("从站 %1 信息读取失败，跳过").arg(i));
            isMotor_[i] = false;
            continue;
        }
        s.vendorId    = si.vendor_id;
        s.productCode = si.product_code;
        s.revisionNumber = si.revision_number;

        bool isMotor;
        if (useMaskHint) {
            isMotor = cfg_.motorMask[i];
        } else {
            isMotor = (si.vendor_id == cfg_.vendor && si.product_code == cfg_.product);
        }
        isMotor_[i] = isMotor;
        s.isMotor   = isMotor;

        if (isMotor) {
            /* 主站进入周期后自动推进 CiA402 使能状态机。hasTarget=false
               会在 Operation Enabled 瞬间捕获实际位置并原位保持，不会跳到零点。 */
            cmds_[i].opMode = MODE_CSP;
            cmds_[i].enable = true;
            cmds_[i].hasTarget = false;
            const char *xml = xmlForRevision(si.revision_number);
            if (!xml) {
                emit errorOccurred(QString("从站 %1 的电机版本 RevisionNo=%2 不受支持；"
                                            "只能绑定 V143 或 V145 XML")
                                     .arg(i).arg(si.revision_number));
                return false;
            }
            emit logMessage(QString("从站 %1: V%2 -> 绑定 xml/%3")
                            .arg(i).arg(si.revision_number).arg(xml));
            /* 仅为电机从站创建 slave_config 并下发 PDO，非电机从站不做任何配置，
               IgH 就不会试图把它拉到 OP，从而不会阻塞电机 activate 过程 */
            sc_[i] = ecrt_master_slave_config(master_, 0, i, si.vendor_id, si.product_code);
            if (!sc_[i]) {
                emit errorOccurred(QString("从站 %1 配置失败 (vendor=0x%2 product=0x%3)")
                    .arg(i)
                    .arg(si.vendor_id, 8, 16, QChar('0'))
                    .arg(si.product_code, 8, 16, QChar('0')));
                return false;
            }
            if (ecrt_slave_config_pdos(sc_[i], EC_END, g_syncs)) {
                emit errorOccurred(QString("从站 %1 PDO 配置失败").arg(i));
                return false;
            }
            motorList += QString(" #%1").arg(i);
        } else {
            sc_[i] = nullptr;
            otherList += QString(" #%1(0x%2/0x%3 %4)")
                .arg(i)
                .arg(si.vendor_id, 0, 16)
                .arg(si.product_code, 0, 16)
                .arg(QString::fromUtf8(si.name));
        }
    }

    int motorCount = 0;
    for (int i = 0; i < N; ++i) if (isMotor_[i]) ++motorCount;
    emit logMessage(QString("识别电机从站 %1 个:%2")
        .arg(motorCount).arg(motorList.isEmpty() ? " (无)" : motorList));
    if (!otherList.isEmpty())
        emit logMessage(QString("其他非电机设备:%1").arg(otherList));
    if (motorCount == 0) {
        emit errorOccurred("未识别到任何电机从站，请检查 Vendor/Product 是否与总线匹配");
        return false;
    }

    /* 仅为电机从站注册 PDO 条目 */
    QVector<ec_pdo_entry_reg_t> regs;
    regs.reserve(motorCount * 13 + 1);
    for (int i = 0; i < N; ++i) {
        if (!isMotor_[i]) continue;
        uint32_t vid = status_[i].vendorId;
        uint32_t pid = status_[i].productCode;
        regs.push_back({0,(uint16_t)i,vid,pid,0x6040,0,&off_cw_[i],  nullptr});
        regs.push_back({0,(uint16_t)i,vid,pid,0x607A,0,&off_tp_[i],  nullptr});
        regs.push_back({0,(uint16_t)i,vid,pid,0x60FF,0,&off_tv_[i],  nullptr});
        regs.push_back({0,(uint16_t)i,vid,pid,0x6071,0,&off_tt_[i],  nullptr});
        regs.push_back({0,(uint16_t)i,vid,pid,0x6060,0,&off_om_[i],  nullptr});
        regs.push_back({0,(uint16_t)i,vid,pid,0x60C2,1,&off_r1_[i],  nullptr});
        regs.push_back({0,(uint16_t)i,vid,pid,0x6041,0,&off_sw_[i],  nullptr});
        regs.push_back({0,(uint16_t)i,vid,pid,0x6064,0,&off_ap_[i],  nullptr});
        regs.push_back({0,(uint16_t)i,vid,pid,0x606C,0,&off_av_[i],  nullptr});
        regs.push_back({0,(uint16_t)i,vid,pid,0x6077,0,&off_at_[i],  nullptr});
        regs.push_back({0,(uint16_t)i,vid,pid,0x6061,0,&off_omd_[i], nullptr});
        regs.push_back({0,(uint16_t)i,vid,pid,0x603F,0,&off_err_[i], nullptr});
        regs.push_back({0,(uint16_t)i,vid,pid,0x2026,0,&off_r2_[i],  nullptr});
    }
    regs.push_back({});
    if (ecrt_domain_reg_pdo_entry_list(domain_, regs.data())) {
        emit errorOccurred("ecrt_domain_reg_pdo_entry_list 失败");
        return false;
    }

    if (ecrt_master_activate(master_)) {
        emit errorOccurred("ecrt_master_activate 失败");
        return false;
    }
    domain_pd_ = ecrt_domain_data(domain_);
    if (!domain_pd_) { emit errorOccurred("domain data 未就绪"); return false; }
    /* 与 EU 官方 demo 一致：激活后先清零整个过程数据区，避免上次进程残值影响 SM watchdog。 */
    std::memset(domain_pd_, 0, ecrt_domain_size(domain_));

    /* 等待所有电机从站进入 OP（非电机位置未配置，不会被 IgH 尝试带到 OP，不阻塞本流程） */
    emit logMessage("等待电机从站进入 OP ...");
    const int startupSleepUs = 500;       // EU demo 建议启动握手快于正常 PDO 周期
    const int firstRetryTick = 20000;     // 10 s 时触发一次 IgH slave-config FSM 重试
    const int maxTicks = 50000;           // 总计最多约 25 s
    QElapsedTimer startupTimer; startupTimer.start();
    for (int tick = 0; tick < maxTicks && running_; ++tick) {
        ecrt_master_receive(master_);
        ecrt_domain_process(domain_);
        ecrt_domain_queue(domain_);
        ecrt_master_send(master_);
        if (tick == firstRetryTick) {
            int rc = ecrt_master_reset(master_);
            emit logMessage(QString("从站未全部 OP，触发 IgH 配置状态机重试 ecrt_master_reset rc=%1").arg(rc));
        }
        if (tick % 1000 == 999 || tick == 0) {
            QString prog;
            int opCnt = 0;
            ec_slave_config_state_t st{};
            for (int i = 0; i < N; ++i) {
                if (!isMotor_[i] || !sc_[i]) continue;
                ecrt_slave_config_state(sc_[i], &st);
                prog += QString(" #%1=AL0x%2/%3")
                    .arg(i).arg(st.al_state, 2, 16, QChar('0'))
                    .arg(st.online ? "on" : "off");
                if (st.al_state == EC_AL_STATE_OP) ++opCnt;
            }
            ec_master_state_t ms{}; ecrt_master_state(master_, &ms);
            ec_domain_state_t ds{}; ecrt_domain_state(domain_, &ds);
            emit logMessage(QString("电机进度 t=%1ms 已OP=%2/%3 master响应=%4 AL汇总=0x%5 "
                                    "domainWC=%6/%7 |%8")
                .arg(startupTimer.elapsed()).arg(opCnt).arg(motorCount)
                .arg(ms.slaves_responding).arg(ms.al_states, 1, 16)
                .arg(ds.working_counter).arg((int)ds.wc_state).arg(prog));
            if (opCnt == motorCount) {
                emit logMessage(QString("全部 %1 个电机从站进入 OP").arg(motorCount));
                return true;
            }
        }
        usleep(startupSleepUs);
    }
    /* 超时，打印最终状态 */
    {
        QString prog;
        ec_slave_config_state_t st{};
        for (int i = 0; i < N; ++i) {
            if (!isMotor_[i] || !sc_[i]) continue;
            ecrt_slave_config_state(sc_[i], &st);
            prog += QString(" #%1 AL=0x%2 online=%3 operational=%4")
                .arg(i).arg(st.al_state, 2, 16, QChar('0'))
                .arg(st.online).arg(st.operational);
        }
        ec_master_state_t ms{}; ecrt_master_state(master_, &ms);
        ec_domain_state_t ds{}; ecrt_domain_state(domain_, &ds);
        emit errorOccurred(QString("超时：约 %1ms 后电机从站未全部进入 OP；master响应=%2 "
                                   "AL汇总=0x%3 domainWC=%4/%5；%6")
            .arg(startupTimer.elapsed()).arg(ms.slaves_responding)
            .arg(ms.al_states, 1, 16).arg(ds.working_counter).arg((int)ds.wc_state).arg(prog));
    }
    return false;
}

void EcWorker::cleanup() {
    if (master_) {
        ecrt_master_deactivate(master_);
        ecrt_release_master(master_);
        master_ = nullptr;
    }
    domain_ = nullptr;
    domain_pd_ = nullptr;
    sc_.clear();
}

/* ============== SDO 处理（在 SDO 子线程调用；阻塞安全） ============== */
void EcWorker::doOneSdo(const SdoJob &job) {
    SdoResult res; res.tag = job.tag;
    if (!master_ || job.slave < 0 || job.slave >= cfg_.slaveCount) {
        res.ok = false; res.err = "主站未启动或从站索引无效";
        emit sdoFinished(res); return;
    }
    uint32_t abort = 0;
    if (job.write) {
        uint8_t buf[8] = {0};
        size_t n = job.bits / 8;
        int64_t v = job.value;
        for (size_t k = 0; k < n; ++k) buf[k] = (v >> (8 * k)) & 0xFF;
        int r = ecrt_master_sdo_download(master_, job.slave, job.index, job.subindex,
                                         buf, n, &abort);
        if (r) { res.ok = false; res.err = QString("SDO写失败 abort=0x%1").arg(abort, 0, 16); }
        else   { res.ok = true; res.value = job.value; }
    } else {
        uint8_t buf[8] = {0};
        size_t n = job.bits / 8;
        size_t actual = 0;
        int r = ecrt_master_sdo_upload(master_, job.slave, job.index, job.subindex,
                                       buf, n, &actual, &abort);
        if (r) { res.ok = false; res.err = QString("SDO读失败 abort=0x%1").arg(abort, 0, 16); }
        else {
            uint64_t u = 0;
            for (size_t k = 0; k < actual; ++k) u |= ((uint64_t)buf[k]) << (8 * k);
            if (job.isSigned) {
                int bits = job.bits;
                int64_t s = (int64_t)u;
                if (bits < 64 && (u & (1ULL << (bits - 1))))
                    s = (int64_t)(u | (~0ULL << bits));
                res.value = s;
            } else res.value = (int64_t)u;
            res.ok = true;
        }
    }
    emit sdoFinished(res);
}

/* ================= 主循环 ================= */
void EcWorker::run() {
    running_ = true;

    /* --- 提高 RT 线程实时性 --- */
    {
        struct sched_param sp; sp.sched_priority = 80;
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (rc == 0) {
            emit logMessage("RT 线程已设置 SCHED_FIFO priority=80");
        } else {
            emit logMessage(QString("<警告> SCHED_FIFO 设置失败(rc=%1)，请使用 sudo 运行，或检查 /etc/security/limits.conf 中 rtprio 限制").arg(rc));
        }
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
            emit logMessage("<警告> mlockall 失败，可能出现页错误导致抖动");
        }
    }

    if (!initMaster()) { cleanup(); running_ = false; emit masterStopped(); return; }
    emit masterStarted();

    /* 启动 SDO 子线程 */
    sdoRun_ = true;
    sdoThread_ = new SdoThread(this);
    sdoThread_->start();

    const int periodUs = cfg_.cycleUs > 0 ? cfg_.cycleUs : 1000;
    struct timespec wakeup; clock_gettime(CLOCK_MONOTONIC, &wakeup);

    int diagTick = 0;
    const int diagPeriod = 1000000 / periodUs;   // 每 1 秒诊断一次

    /* 周期计时统计：实际周期 = 相邻两次唤醒后的时间差；
       负载时间 = PDO收/算/发 所占用时间                  */
    struct timespec tPrev{}; bool tPrevValid = false;
    long sumDtNs = 0, maxDtNs = 0, minDtNs = LONG_MAX;
    long sumLoadNs = 0, maxLoadNs = 0;
    int  cycCnt = 0;
    long overrunCnt = 0;
    const long thresholdNs = (long)periodUs * 1000L * 3 / 2;  // 超过 1.5× 视为 overrun
    int stopCycles = 0;

    while (running_) {
        /* 等到下一个周期 */
        wakeup.tv_nsec += (long)periodUs * 1000L;
        while (wakeup.tv_nsec >= 1000000000L) { wakeup.tv_nsec -= 1000000000L; wakeup.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wakeup, nullptr);

        /* --- 周期测量：唤醒时刻 --- */
        struct timespec tWake; clock_gettime(CLOCK_MONOTONIC, &tWake);
        if (tPrevValid) {
            long dt = (tWake.tv_sec - tPrev.tv_sec) * 1000000000L
                    + (tWake.tv_nsec - tPrev.tv_nsec);
            sumDtNs += dt;
            if (dt > maxDtNs) maxDtNs = dt;
            if (dt < minDtNs) minDtNs = dt;
            if (dt > thresholdNs) overrunCnt++;
            cycCnt++;
        }
        tPrev = tWake; tPrevValid = true;

        ecrt_master_receive(master_);
        ecrt_domain_process(domain_);

        /* 读快照 + 写入控制 */
        QVector<MotorCommand> cmdLocal;
        {
            QMutexLocker lk(&mtx_);
            cmdLocal = cmds_;
        }

        MotionMode activeMotion;
        {
            QMutexLocker ml(&motionMtx_);
            activeMotion = motionMode_;
        }
        const bool stopping = stopRequested_;
        if ((activeMotion == MotionReturning || activeMotion == MotionReady || activeMotion == MotionPlaying)
                && motionTarget_.size() != cfg_.slaveCount) {
            motionTarget_.fill(0, cfg_.slaveCount);
            for (int i = 0; i < cfg_.slaveCount; ++i)
                if (isMotor_[i]) motionTarget_[i] = *(int32_t*)(domain_pd_ + off_ap_[i]);
        }

        QVector<MotorStatus> stLocal(cfg_.slaveCount);
        for (int i = 0; i < cfg_.slaveCount; ++i) {
            /* 非电机位置：仅刷新 AL/在线信息，不读写 PDO、不运行状态机 */
            if (!isMotor_[i]) {
                MotorStatus s;
                s.isMotor     = false;
                s.vendorId    = status_[i].vendorId;
                s.productCode = status_[i].productCode;
                /* 非电机未通过 ecrt_master_slave_config 注册，只能通过 master 查询实时 info */
                ec_slave_info_t si{};
                if (ecrt_master_get_slave(master_, (uint16_t)i, &si) == 0) {
                    s.alState = si.al_state;
                    s.online  = (si.al_state != 0);
                }
                stLocal[i] = s;
                continue;
            }

            uint16_t sw  = *(uint16_t*)(domain_pd_ + off_sw_[i]);
            int32_t  ap  = *(int32_t*) (domain_pd_ + off_ap_[i]);
            int32_t  av  = *(int32_t*) (domain_pd_ + off_av_[i]);
            int16_t  at  = *(int16_t*) (domain_pd_ + off_at_[i]);
            int8_t   omd = *(int8_t*)  (domain_pd_ + off_omd_[i]);
            uint16_t er  = *(uint16_t*)(domain_pd_ + off_err_[i]);

            ec_slave_config_state_t cs{};
            ecrt_slave_config_state(sc_[i], &cs);

            MotorStatus s;
            s.statusWord=sw; s.actualPos=ap; s.actualVel=av;
            s.actualTor=at; s.opModeDisp=omd; s.errorCode=er;
            s.alState=cs.al_state; s.online=cs.online;
            s.isMotor = true;
            s.vendorId    = status_[i].vendorId;
            s.productCode = status_[i].productCode;
            stLocal[i]=s;

            /* ----- CiA402 状态机 ----- */
            MotorCommand &c = cmdLocal[i];
            if (stopping) {
                /* 退出时逐轴保留原使能状态：已失能轴继续 Shutdown，
                   已使能或正在执行轨迹的轴改用 CSP 锁定当前反馈位置，
                   等待实际速度归零后保持 Operation Enabled 释放主站。 */
                const bool trajectoryActive = activeMotion == MotionReturning
                                           || activeMotion == MotionReady
                                           || activeMotion == MotionPlaying;
                const bool keepEnabled = c.enable || trajectoryActive;
                *(int8_t*)  (domain_pd_ + off_om_[i]) = MODE_CSP;
                *(int32_t*) (domain_pd_ + off_tp_[i]) = ap;
                *(int32_t*) (domain_pd_ + off_tv_[i]) = 0;
                *(int16_t*) (domain_pd_ + off_tt_[i]) = 0;
                uint16_t holdCtrl = 0x06; // 原为失能请求时绝不重新使能
                if (keepEnabled) {
                    switch (sw & 0x6F) {
                        case 0x00: case 0x40: holdCtrl = 0x06; break;
                        case 0x21:            holdCtrl = 0x07; break;
                        case 0x23: case 0x27: case 0x07: holdCtrl = 0x0F; break;
                        case 0x0F:            holdCtrl = 0x00; break;
                        case 0x08:            holdCtrl = 0x80; break;
                        default:              holdCtrl = 0x06; break;
                    }
                }
                *(uint16_t*)(domain_pd_ + off_cw_[i]) = holdCtrl;
                ppPhase_[i] = 0;
                continue;
            }
            if (activeMotion == MotionRecording) {
                c.enable = false;
            } else if (activeMotion == MotionReturning || activeMotion == MotionReady || activeMotion == MotionPlaying) {
                c.enable = true;
                c.opMode = MODE_CSP;
            }
            *(int8_t*)(domain_pd_ + off_om_[i]) = c.opMode;

            bool prevEnabled = enabled_[i];
            uint16_t ctrl = 0x06;
            if (sdoSafe_) {
                /* 调参模式：全部电机下电，禁用脉冲与运动，仅维持通讯 */
                ctrl = 0x06;
                enabled_[i] = false;
                ppPhase_[i] = 0;
            } else if (c.faultReset) {
                ctrl = 0x80;
                /* 消费一次后清零 */
                QMutexLocker lk(&mtx_);
                if (i < cmds_.size()) cmds_[i].faultReset = false;
            } else if (!c.enable) {
                ctrl = 0x06;                  // Shutdown
                enabled_[i] = false;
            } else {
                switch (sw & 0x6F) {
                    case 0x00: case 0x40: ctrl = 0x06; break;           // Not ready -> Shutdown
                    case 0x21:            ctrl = 0x07; break;           // Ready -> SwitchOn
                    case 0x23:            ctrl = 0x0F; break;           // SwitchedOn -> EnableOperation
                    case 0x27:            ctrl = 0x0F; enabled_[i]=true; break; // Operation enabled
                    case 0x07:            ctrl = 0x0F; break;           // QuickStopActive -> EnableOperation
                    case 0x0F:            ctrl = 0x00; break;           // FaultReactionActive: 等待进入 Fault
                    case 0x08:            ctrl = 0x80; break;           // Fault -> FaultReset
                    default:              ctrl = 0x06; break;
                }
            }

            /* 进入使能瞬间，捕获当前位置作为基准，避免 CSP 目标跳变 */
            if (!prevEnabled && enabled_[i]) {
                startPos_[i] = ap;
                curTp_[i]    = ap;
                lastSeq_[i]  = c.applySeq;   // 忽略使能前的残留 applySeq，避免误触发
                ppPhase_[i]  = 0;
                emit logMessage(QString("电机 %1 进入 OperationEnabled，起始位置=%2").arg(i).arg(ap));
            }

            /* 目标值 & 按模式定制 control word */
            int32_t tp = ap;   // 默认跟随实际位置
            if (c.opMode == MODE_CSP || c.opMode == MODE_IP) {
                if (!enabled_[i]) {
                    tp = ap;
                    curTp_[i] = ap;
                } else if (!c.absolute) {
                    curTp_[i] += c.jogStep;
                    tp = curTp_[i];
                } else if (c.hasTarget) {
                    int32_t goal = startPos_[i] + c.targetPos;
                    int32_t step = c.jogStep > 0 ? c.jogStep : 500;
                    int32_t diff = goal - curTp_[i];
                    if (diff > step)       curTp_[i] += step;
                    else if (diff < -step) curTp_[i] -= step;
                    else                   curTp_[i]  = goal;
                    tp = curTp_[i];
                } else {
                    tp = startPos_[i];
                    curTp_[i] = startPos_[i];
                }
            } else if (!enabled_[i]) {
                tp = ap;
            } else {
                /* PP/PV/HM/CST/CSV：直接下发用户目标 */
                tp = c.targetPos;
            }
            *(int32_t*) (domain_pd_ + off_tp_[i]) = tp;
            *(int32_t*) (domain_pd_ + off_tv_[i]) = c.targetVel;
            *(int16_t*) (domain_pd_ + off_tt_[i]) = c.targetTor;

            /* 机械臂轨迹对 CSP 绝对目标拥有最高优先级，所有轴共用同一 playTick。 */
            if (activeMotion == MotionReturning) {
                if (enabled_[i]) {
                    const int32_t goal = playback_.frames.first()[i];
                    const int64_t maxStep64 = qMax<int64_t>(1, (int64_t)returnSpeed_ * periodUs / 1000000);
                    const int32_t maxStep = (int32_t)qMin<int64_t>(maxStep64, INT_MAX);
                    const int64_t diff = (int64_t)goal - motionTarget_[i];
                    if (diff > maxStep) motionTarget_[i] += maxStep;
                    else if (diff < -maxStep) motionTarget_[i] -= maxStep;
                    else motionTarget_[i] = goal;
                } else {
                    motionTarget_[i] = ap;
                }
                *(int32_t*)(domain_pd_ + off_tp_[i]) = motionTarget_[i];
            } else if (activeMotion == MotionReady) {
                motionTarget_[i] = playback_.frames.first()[i];
                *(int32_t*)(domain_pd_ + off_tp_[i]) = motionTarget_[i];
            } else if (activeMotion == MotionPlaying) {
                const int64_t elapsedUs = (int64_t)playTick_ * periodUs;
                const int64_t frameUs = (int64_t)playback_.sampleMs * 1000;
                int a = (int)(elapsedUs / frameUs);
                if (a >= playback_.frames.size() - 1) a = playback_.frames.size() - 1;
                int b = qMin(a + 1, playback_.frames.size() - 1);
                const int64_t rem = elapsedUs - (int64_t)a * frameUs;
                const int64_t p0 = playback_.frames[a][i], p1 = playback_.frames[b][i];
                motionTarget_[i] = (int32_t)(p0 + (p1 - p0) * rem / frameUs);
                *(int32_t*)(domain_pd_ + off_tp_[i]) = motionTarget_[i];
            }

            /* ===== PP / PV / HM：new_setpoint 脉冲握手 =====
               - 用户每点"应用目标值" applySeq 会自增
               - 收到新 seq 时进入 phase1：CW bit4=1 (new setpoint) + bit5=1 (change set immediately)
                 （PP 还用 bit6 选 abs/rel：absolute=true -> bit6=0 绝对；false -> bit6=1 相对）
               - 等待 SW bit12 (setpoint_ack) = 1 → phase2：CW bit4=0
               - 等待 SW bit12 = 0 → 完成 phase0                                              */
            if (enabled_[i] && (c.opMode == MODE_PP || c.opMode == MODE_PV || c.opMode == MODE_HM)) {
                if (c.applySeq != lastSeq_[i]) {
                    ppPhase_[i] = 1;
                    lastSeq_[i] = c.applySeq;
                    emit logMessage(QString("电机 %1 触发 new_setpoint (mode=%2, target=%3)")
                        .arg(i).arg((int)c.opMode).arg(c.targetPos));
                }
                bool ack = (sw & (1 << 12)) != 0;
                if (ppPhase_[i] == 1) {
                    ctrl |= (1 << 4);           // new setpoint
                    ctrl |= (1 << 5);           // change set immediately
                    if (c.opMode == MODE_PP && !c.absolute) ctrl |= (1 << 6); // relative
                    if (ack) ppPhase_[i] = 2;
                } else if (ppPhase_[i] == 2) {
                    /* bit4 清零等 ack 回落 */
                    if (c.opMode == MODE_PP && !c.absolute) ctrl |= (1 << 6);
                    if (!ack) ppPhase_[i] = 0;
                }
            } else {
                ppPhase_[i] = 0;
            }

            /* IP 模式：使能后持续置 CW bit4 = 激活插补 (手册 4.4 CW=0x1F) */
            if (enabled_[i] && c.opMode == MODE_IP) {
                ctrl |= (1 << 4);
            }

            *(uint16_t*)(domain_pd_ + off_cw_[i]) = ctrl;
        }

        {
            QMutexLocker lk(&mtx_);
            status_ = stLocal;
        }


        if (activeMotion == MotionRecording) {
            const int every = qMax(1, recorded_.sampleMs * 1000 / periodUs);
            if (recordTick_++ % every == 0) {
                QVector<int32_t> frame(cfg_.slaveCount, 0);
                for (int i = 0; i < cfg_.slaveCount; ++i)
                    if (isMotor_[i]) frame[i] = stLocal[i].actualPos;
                QMutexLocker ml(&motionMtx_);
                if (motionMode_ == MotionRecording) recorded_.frames.push_back(frame);
            }
        } else if (activeMotion == MotionReturning) {
            bool reached = true, allEnabled = true;
            for (int i = 0; i < cfg_.slaveCount; ++i) if (isMotor_[i]) {
                allEnabled &= enabled_[i];
                const int64_t actualError = (int64_t)stLocal[i].actualPos - playback_.frames.first()[i];
                /* 目标发生器到位且实际位置误差不超过 100 encoder counts 才算真正回到起点。 */
                reached &= (motionTarget_[i] == playback_.frames.first()[i]
                         && qAbs(actualError) <= 100);
            }
            if (allEnabled && reached) {
                QMutexLocker ml(&motionMtx_);
                if (motionMode_ == MotionReturning) {
                    motionMode_ = MotionReady;
                    playTick_ = 0;
                    emit motionStateChanged("已到起点");
                }
            }
        } else if (activeMotion == MotionPlaying) {
            const int64_t totalUs = (int64_t)(playback_.frames.size() - 1) * playback_.sampleMs * 1000;
            if ((int64_t)playTick_ * periodUs >= totalUs) {
                QMutexLocker ml(&motionMtx_);
                if (motionMode_ == MotionPlaying) {
                    motionMode_ = MotionIdle;
                    {
                        QMutexLocker lk(&mtx_);
                        for (int i = 0; i < cmds_.size(); ++i) {
                            if (!isMotor_[i]) continue;
                            const int32_t finalPosition = playback_.frames.last()[i];
                            startPos_[i] = finalPosition;
                            curTp_[i] = finalPosition;
                            cmds_[i].opMode = MODE_CSP;
                            cmds_[i].enable = true;
                            cmds_[i].hasTarget = false;
                        }
                    }
                    emit motionStateChanged("播放完成");
                }
            } else ++playTick_;
        }

        ecrt_domain_queue(domain_);
        ecrt_master_send(master_);

        if (stopping) {
            bool allStopped = true;
            for (int i = 0; i < cfg_.slaveCount; ++i) {
                if (isMotor_[i] && qAbs((int)stLocal[i].actualVel) > 10) {
                    allStopped = false;
                    break;
                }
            }
            if (++stopCycles == 1)
                emit logMessage("退出请求：保留各轴原使能/失能状态；运动轴已用 CSP 锁定当前位置，等待速度归零");
            /* 至少发送 3 个周期；异常时最多等待 1 秒，避免退出无限阻塞。 */
            if (stopCycles >= 3 && (allStopped || stopCycles >= 1000000 / periodUs))
                running_ = false;
        }

        /* --- 本周期负载时间 --- */
        struct timespec tEnd; clock_gettime(CLOCK_MONOTONIC, &tEnd);
        {
            long load = (tEnd.tv_sec - tWake.tv_sec) * 1000000000L
                      + (tEnd.tv_nsec - tWake.tv_nsec);
            sumLoadNs += load;
            if (load > maxLoadNs) maxLoadNs = load;
        }

        /* 每秒诊断日志：CSP 模式下打印 实际/下发/终点，帮助排查不动的原因 */
        if (++diagTick >= diagPeriod) {
            diagTick = 0;

            if (cycCnt > 0) {
                double avgUs = (double)sumDtNs / cycCnt / 1000.0;
                double minUs = (double)minDtNs / 1000.0;
                double maxUs = (double)maxDtNs / 1000.0;
                double jitterUs = maxUs - minUs;
                double loadAvgUs = (double)sumLoadNs / cycCnt / 1000.0;
                double loadMaxUs = (double)maxLoadNs / 1000.0;
                emit logMessage(QString(
                    "周期诊断 目标=%1us 实测 avg=%2us min=%3us max=%4us 抖动=%5us | 负载 avg=%6us max=%7us | overrun=%8/%9")
                    .arg(periodUs)
                    .arg(avgUs, 0, 'f', 2).arg(minUs, 0, 'f', 2)
                    .arg(maxUs, 0, 'f', 2).arg(jitterUs, 0, 'f', 2)
                    .arg(loadAvgUs, 0, 'f', 2).arg(loadMaxUs, 0, 'f', 2)
                    .arg(overrunCnt).arg(cycCnt));
            }
            sumDtNs = 0; maxDtNs = 0; minDtNs = LONG_MAX;
            sumLoadNs = 0; maxLoadNs = 0;
            cycCnt = 0; overrunCnt = 0;

            for (int i = 0; i < cfg_.slaveCount; ++i) {
                if (!enabled_[i]) continue;
                const MotorCommand &c = cmdLocal[i];
                if (c.opMode != MODE_CSP || !c.hasTarget) continue;
                int32_t ap   = *(int32_t*)(domain_pd_ + off_ap_[i]);
                int32_t goal = startPos_[i] + c.targetPos;
                emit logMessage(QString("CSP诊断 电机%1 SW=0x%2 AP=%3 curTP=%4 GOAL=%5 step=%6")
                    .arg(i)
                    .arg(*(uint16_t*)(domain_pd_ + off_sw_[i]), 4, 16, QChar('0'))
                    .arg(ap).arg(curTp_[i]).arg(goal)
                    .arg(c.jogStep > 0 ? c.jogStep : 500));
            }
            if (activeMotion == MotionReturning) {
                QString detail;
                for (int i = 0; i < cfg_.slaveCount; ++i) if (isMotor_[i]) {
                    detail += QString(" #%1 SW=0x%2 EN=%3 AP=%4 TP=%5 START=%6 ERR=%7")
                        .arg(i).arg(stLocal[i].statusWord, 4, 16, QChar('0'))
                        .arg(enabled_[i]).arg(stLocal[i].actualPos).arg(motionTarget_[i])
                        .arg(playback_.frames.first()[i])
                        .arg((int64_t)stLocal[i].actualPos - playback_.frames.first()[i]);
                }
                emit logMessage("回起点诊断 |" + detail);
            }
        }
    }

    /* 停止 SDO 子线程 */
    sdoRun_ = false;
    { QMutexLocker lk(&sdoMtx_); sdoCv_.wakeAll(); }
    if (sdoThread_) {
        /* 必须无限等待：若 SDO 仍卡在 ecrt_master_sdo_* 的内核调用里，
           我们宁愿多等几秒，也绝不能在 master 释放前让 SDO 线程悬挂 */
        sdoThread_->wait();
        delete sdoThread_;
        sdoThread_ = nullptr;
    }

    cleanup();
    emit masterStopped();
}
