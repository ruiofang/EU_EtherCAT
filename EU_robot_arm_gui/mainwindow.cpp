#include "mainwindow.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTimer>
#include <QCheckBox>
#include <QDateTime>
#include <QMessageBox>
#include <QIntValidator>
#include <QCloseEvent>
#include <QApplication>
#include <QSplitter>
#include <QScrollArea>
#include <QGridLayout>
#include <QStyle>
#include <QListWidget>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QStandardPaths>
#include <climits>

extern "C" {
#include <ecrt.h>
}

static QString alStateText(uint8_t s) {
    switch (s) {
        case 0x01: return "INIT";
        case 0x02: return "PREOP";
        case 0x03: return "BOOT";
        case 0x04: return "SAFEOP";
        case 0x08: return "OP";
        default:   return QString("0x%1").arg(s, 2, 16, QChar('0'));
    }
}

static QString swText(uint16_t sw) {
    QString s;
    if (sw & (1<<3)) s += "FAULT ";
    switch (sw & 0x6F) {
        case 0x00: s += "NotReady";            break;
        case 0x40: s += "SwitchOnDisabled";    break;
        case 0x21: s += "ReadyToSwitchOn";     break;
        case 0x23: s += "SwitchedOn";          break;
        case 0x27: s += "OperationEnabled";    break;
        case 0x07: s += "QuickStopActive";     break;
        case 0x0F: s += "FaultReactionActive"; break;
        case 0x08: s += "Fault";               break;
        default:   s += QString("0x%1").arg(sw, 4, 16, QChar('0'));
    }
    return s;
}

static MotorCommand buildCmdFromPanel(const MotorPanel &p);

/* ---------------------------------------------------------------- */
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    qRegisterMetaType<RecordedMotion>("RecordedMotion");
    buildUi();
    loadMotionLibrary();
    rebuildMotorTabs(spSlaves_->value());

    timer_ = new QTimer(this);
    timer_->setInterval(50);    // 20Hz 刷新
    connect(timer_, &QTimer::timeout, this, &MainWindow::onRefreshStatus);

    /* 启动后自动扫描一次，让"从站数/vendor/product"与实际总线一致 */
    QTimer::singleShot(0, this, &MainWindow::onScan);
}

MainWindow::~MainWindow() {
    if (worker_) {
        worker_->requestStop();
        /* 必须无限等待 worker 真正结束：否则 RT 线程仍在内核 ioctl 中，
           进程即使 exit 也会留下僵尸持有 /dev/EtherCAT0，造成模块无法卸载 */
        worker_->wait();
        delete worker_;
        worker_ = nullptr;
    }
}

/* 关窗口前保证主站被干净地关闭 */
void MainWindow::closeEvent(QCloseEvent *event) {
    if (worker_ && running_) {
        appendLog("窗口关闭：先停止主站 ...");
        /* 禁用 UI，避免用户再次点击引发重入 */
        setEnabled(false);
        QApplication::processEvents();
        worker_->requestStop();
        worker_->wait();          // 必然等到 cleanup 完成
    }
    event->accept();
}

/* ---------------------------------------------------------------- */
void MainWindow::buildUi() {
    setWindowTitle("EU EtherCAT 机械臂动作录制与播放");
    /* 初始尺寸适中；低分辨率(1366x768)也能放下，最小尺寸给足滚动空间 */
    resize(1100, 720);
    setMinimumSize(900, 560);

    auto *central = new QWidget(this);
    auto *root    = new QVBoxLayout(central);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(4);

    /* ========= 顶部：主站配置（GridLayout 窄屏自动换行） ========= */
    auto *gbTop = new QGroupBox("主站配置", central);
    auto *top   = new QGridLayout(gbTop);
    top->setHorizontalSpacing(8);
    top->setVerticalSpacing(4);

    spSlaves_ = new QSpinBox;  spSlaves_->setRange(1, 64); spSlaves_->setValue(1);
    spSlaves_->setMinimumWidth(60);
    edVendor_ = new QLineEdit("0x00001097"); edVendor_->setMinimumWidth(110);
    edProduct_= new QLineEdit("0x00002406"); edProduct_->setMinimumWidth(110);
    spCycleUs_= new QSpinBox;  spCycleUs_->setRange(250, 20000); spCycleUs_->setValue(1000);
    spCycleUs_->setSuffix(" us"); spCycleUs_->setMinimumWidth(90);

    btnStart_ = new QPushButton("启动主站");
    btnStop_  = new QPushButton("停止主站"); btnStop_->setEnabled(false);
    lblMasterState_ = new QLabel("● 未启动");
    lblMasterState_->setStyleSheet("color:gray;font-weight:bold;");

    btnSdoSafe_ = new QPushButton("SDO 调参模式(下电)");
    btnSdoSafe_->setCheckable(true);
    btnSdoSafe_->setEnabled(false);
    btnSdoSafe_->setToolTip("启用后所有电机自动下电，仅维持通讯用于 SDO 读写；关闭后恢复 PDO 控制");

    btnScan_ = new QPushButton("扫描从站");
    btnScan_->setToolTip("检测总线上在线的从站数量和 vendor/product，自动填入并重建电机面板");

    /* 第 0 行：参数 */
    top->addWidget(new QLabel("从站数:"), 0, 0); top->addWidget(spSlaves_,  0, 1);
    top->addWidget(new QLabel("Vendor:"),  0, 2); top->addWidget(edVendor_,  0, 3);
    top->addWidget(new QLabel("Product:"), 0, 4); top->addWidget(edProduct_, 0, 5);
    top->addWidget(new QLabel("周期:"),    0, 6); top->addWidget(spCycleUs_, 0, 7);
    /* 第 1 行：按钮 */
    top->addWidget(btnScan_,        1, 0, 1, 2);
    top->addWidget(btnStart_,       1, 2);
    top->addWidget(btnStop_,        1, 3);
    top->addWidget(btnSdoSafe_,     1, 4, 1, 2);
    top->addWidget(lblMasterState_, 1, 6, 1, 2);
    top->setColumnStretch(7, 1);

    connect(spSlaves_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int n){ if (!running_) rebuildMotorTabs(n); });
    connect(btnStart_, &QPushButton::clicked, this, &MainWindow::onStart);
    connect(btnStop_,  &QPushButton::clicked, this, &MainWindow::onStop);
    connect(btnSdoSafe_, &QPushButton::toggled, this, &MainWindow::onToggleSdoSafe);
    connect(btnScan_,  &QPushButton::clicked, this, &MainWindow::onScan);

    /* ========= 中/下：用 Splitter 让用户按屏幕自由分配 ========= */
    auto *splitter = new QSplitter(Qt::Vertical, central);
    splitter->setChildrenCollapsible(false);

    tabs_ = new QTabWidget(splitter);
    tabs_->setMinimumHeight(240);

    auto *gbLog = new QGroupBox("日志", splitter);
    auto *lg = new QVBoxLayout(gbLog);
    lg->setContentsMargins(4, 4, 4, 4);
    log_ = new QPlainTextEdit; log_->setReadOnly(true);
    log_->setMaximumBlockCount(2000);
    QFont logFont("Monospace"); logFont.setStyleHint(QFont::TypeWriter);
    log_->setFont(logFont);
    log_->setMinimumHeight(100);
    lg->addWidget(log_);

    splitter->addWidget(tabs_);
    splitter->addWidget(gbLog);
    splitter->setStretchFactor(0, 3);   // Tab 默认占 3/4
    splitter->setStretchFactor(1, 1);   // 日志默认占 1/4

    root->addWidget(gbTop);

    /* ========= 机械臂动作示教 ========= */
    auto *gbMotion = new QGroupBox("机械臂动作录制与播放", central);
    auto *motionLay = new QGridLayout(gbMotion);
    motionList_ = new QListWidget;
    motionList_->setMaximumHeight(92);
    motionList_->setToolTip("已保存动作（名称 / 帧数 / 时长）");
    spRecordMs_ = new QSpinBox; spRecordMs_->setRange(5, 200); spRecordMs_->setValue(20); spRecordMs_->setSuffix(" ms");
    spReturnSpeed_ = new QSpinBox; spReturnSpeed_->setRange(100, INT_MAX); spReturnSpeed_->setValue(100000); spReturnSpeed_->setSuffix(" 脉冲/s");
    btnRecord_ = new QPushButton("开始录制（失能）");
    btnEnableAll_ = new QPushButton("使能");
    btnFaultResetAll_ = new QPushButton("一键故障复位");
    btnDisableRelease_ = new QPushButton("失能并松开抱闸");
    btnReturn_ = new QPushButton("回起点");
    btnPlay_ = new QPushButton("播放");
    btnReturnPlay_ = new QPushButton("回起点并播放");
    btnMotionStop_ = new QPushButton("停止动作");
    btnMotionDelete_ = new QPushButton("删除所选");
    lblMotionState_ = new QLabel("空闲");
    motionLay->addWidget(motionList_, 0, 0, 4, 1);
    motionLay->addWidget(new QLabel("采样周期:"), 0, 1); motionLay->addWidget(spRecordMs_, 0, 2);
    motionLay->addWidget(new QLabel("回起点速度:"), 1, 1); motionLay->addWidget(spReturnSpeed_, 1, 2);
    motionLay->addWidget(btnRecord_, 0, 3); motionLay->addWidget(btnMotionDelete_, 0, 4);
    motionLay->addWidget(btnEnableAll_, 1, 3); motionLay->addWidget(btnDisableRelease_, 1, 4);
    motionLay->addWidget(btnFaultResetAll_, 2, 2);
    motionLay->addWidget(btnReturn_, 2, 3); motionLay->addWidget(btnPlay_, 2, 4);
    motionLay->addWidget(btnReturnPlay_, 3, 3); motionLay->addWidget(btnMotionStop_, 3, 4);
    motionLay->addWidget(new QLabel("状态:"), 3, 1); motionLay->addWidget(lblMotionState_, 3, 2, 1, 1);
    motionLay->setColumnStretch(0, 1);
    connect(btnRecord_, &QPushButton::clicked, this, &MainWindow::onRecordToggle);
    connect(btnEnableAll_, &QPushButton::clicked, this, &MainWindow::onEnableAllMotion);
    connect(btnFaultResetAll_, &QPushButton::clicked, this, &MainWindow::onFaultResetAllMotion);
    connect(btnDisableRelease_, &QPushButton::clicked, this, &MainWindow::onDisableReleaseAll);
    connect(btnReturn_, &QPushButton::clicked, this, &MainWindow::onReturnMotion);
    connect(btnReturnPlay_, &QPushButton::clicked, this, &MainWindow::onReturnAndPlayMotion);
    connect(btnPlay_, &QPushButton::clicked, this, &MainWindow::onPlayMotion);
    connect(btnMotionStop_, &QPushButton::clicked, this, &MainWindow::onStopMotion);
    connect(btnMotionDelete_, &QPushButton::clicked, this, &MainWindow::onDeleteMotion);
    root->addWidget(gbMotion);
    root->addWidget(splitter, 1);

    setCentralWidget(central);
}

/* ---------------------------------------------------------------- */
void MainWindow::rebuildMotorTabs(int n) {
    tabs_->clear();
    panels_.clear();
    panels_.resize(n);

    for (int i = 0; i < n; ++i) {
        MotorPanel &p = panels_[i];
        auto *w = new QWidget;
        auto *lay = new QVBoxLayout(w);
        lay->setContentsMargins(6, 6, 6, 6);
        lay->setSpacing(6);

        /* ------- 状态区 ------- */
        auto *gbSt = new QGroupBox(QString("电机 %1 状态").arg(i));
        auto *st = new QFormLayout(gbSt);
        p.lblAl       = new QLabel("-");
        p.lblSw       = new QLabel("-");
        p.lblPos      = new QLabel("0");
        p.lblVel      = new QLabel("0");
        p.lblTor      = new QLabel("0");
        p.lblErr      = new QLabel("0x0000");
        p.lblModeDisp = new QLabel("-");
        QFont mono("Monospace"); mono.setStyleHint(QFont::TypeWriter);
        for (auto *l : {p.lblAl,p.lblSw,p.lblPos,p.lblVel,p.lblTor,p.lblErr,p.lblModeDisp}) l->setFont(mono);
        st->addRow("AL 状态:",    p.lblAl);
        st->addRow("Status Word:",p.lblSw);
        st->addRow("当前位置 (0x6064):", p.lblPos);
        st->addRow("当前速度 (0x606C):", p.lblVel);
        st->addRow("当前力矩 (0x6077):", p.lblTor);
        st->addRow("错误码 (0x603F):",   p.lblErr);
        st->addRow("模式显示 (0x6061):", p.lblModeDisp);

        /* ------- 控制区 ------- */
        auto *gbCtl = new QGroupBox("运动控制");
        auto *fl = new QFormLayout(gbCtl);

        p.cbMode = new QComboBox;
        p.cbMode->addItem("CSP - 周期同步位置 (8)", (int)MODE_CSP);
        p.cbMode->addItem("CSV - 周期同步速度 (9)", (int)MODE_CSV);
        p.cbMode->addItem("CST - 周期同步力矩 (10)",(int)MODE_CST);
        p.cbMode->addItem("PP  - 轮廓位置 (1)",     (int)MODE_PP);
        p.cbMode->addItem("PV  - 轮廓速度 (3)",     (int)MODE_PV);
        p.cbMode->addItem("PT  - 轮廓力矩 (4)",     (int)MODE_PT);
        p.cbMode->addItem("HM  - 回零 (6)",         (int)MODE_HM);
        p.cbMode->addItem("IP  - 插补位置 (7)",     (int)MODE_IP);

        p.spTarget = new QSpinBox;  p.spTarget->setRange(INT_MIN, INT_MAX); p.spTarget->setValue(0);
        p.chkAbs   = new QCheckBox("绝对(相对起始位置偏移,推荐) / 不勾=点动累加"); p.chkAbs->setChecked(true);
        p.spJogStep= new QSpinBox;  p.spJogStep->setRange(-1000000, 1000000); p.spJogStep->setValue(500);
        p.spProfVel= new QSpinBox;  p.spProfVel->setRange(0, INT_MAX); p.spProfVel->setValue(100000);
        p.spProfAcc= new QSpinBox;  p.spProfAcc->setRange(0, INT_MAX); p.spProfAcc->setValue(500000);
        p.spProfDec= new QSpinBox;  p.spProfDec->setRange(0, INT_MAX); p.spProfDec->setValue(500000);
        p.spTorSlope   = new QSpinBox; p.spTorSlope->setRange(0, INT_MAX);      p.spTorSlope->setValue(1000);
        p.spHomeMethod = new QSpinBox; p.spHomeMethod->setRange(-128, 127);     p.spHomeMethod->setValue(35);
        p.spHomeSpdSw  = new QSpinBox; p.spHomeSpdSw->setRange(0, INT_MAX);     p.spHomeSpdSw->setValue(5000);
        p.spHomeSpdZero= new QSpinBox; p.spHomeSpdZero->setRange(0, INT_MAX);   p.spHomeSpdZero->setValue(1000);
        p.spHomeAcc    = new QSpinBox; p.spHomeAcc->setRange(0, INT_MAX);       p.spHomeAcc->setValue(100000);
        p.spHomeOffset = new QSpinBox; p.spHomeOffset->setRange(INT_MIN, INT_MAX); p.spHomeOffset->setValue(0);
        p.spIpTimeMs   = new QSpinBox; p.spIpTimeMs->setRange(1, 250);          p.spIpTimeMs->setValue(1);

        p.btnEnable     = new QPushButton("使能");
        p.btnDisable    = new QPushButton("关闭");
        p.btnFaultReset = new QPushButton("故障复位");
        p.btnApply      = new QPushButton("应用目标值");

        auto *btnRow = new QHBoxLayout;
        btnRow->addWidget(p.btnEnable); btnRow->addWidget(p.btnDisable);
        btnRow->addWidget(p.btnFaultReset); btnRow->addWidget(p.btnApply);

        fl->addRow("操作模式:", p.cbMode);
        fl->addRow("目标值(位置=相对起始偏移/速度/力矩):", p.spTarget);
        fl->addRow("", p.chkAbs);
        fl->addRow("CSP/IP 每周期最大步长(脉冲):", p.spJogStep);
        fl->addRow("轮廓速度 0x6081 (PP):", p.spProfVel);
        fl->addRow("轮廓加速 0x6083 (PP):", p.spProfAcc);
        fl->addRow("轮廓减速 0x6084 (PP):", p.spProfDec);
        fl->addRow("力矩斜率 0x6087 (PT):", p.spTorSlope);
        fl->addRow("回零方法 0x6098 (HM):", p.spHomeMethod);
        fl->addRow("搜索开关速度 0x6099:01 (HM):", p.spHomeSpdSw);
        fl->addRow("搜索零点速度 0x6099:02 (HM):", p.spHomeSpdZero);
        fl->addRow("回零加速度 0x609A (HM):", p.spHomeAcc);
        fl->addRow("原点偏移 0x607C (HM):", p.spHomeOffset);
        fl->addRow("插补周期 0x60C2:01 ms (IP):", p.spIpTimeMs);
        auto *rowW = new QWidget; rowW->setLayout(btnRow);
        fl->addRow(rowW);

        /* ------- 抱闸控制 (0x2014:01 写 / 0x2014:02 读) ------- */
        auto *gbBrake = new QGroupBox("抱闸控制 (0x2014)");
        auto *bl = new QHBoxLayout(gbBrake);
        p.btnBrakeOpen  = new QPushButton("松开抱闸");
        p.btnBrakeClose = new QPushButton("抱死抱闸");
        p.btnBrakeQuery = new QPushButton("刷新状态");
        p.lblBrakeState = new QLabel("-"); p.lblBrakeState->setFont(mono);
        p.btnBrakeOpen->setStyleSheet("background:#bfb;");
        p.btnBrakeClose->setStyleSheet("background:#fbb;");
        bl->addWidget(p.btnBrakeOpen);
        bl->addWidget(p.btnBrakeClose);
        bl->addWidget(p.btnBrakeQuery);
        bl->addWidget(new QLabel("状态(0x2014:02):"));
        bl->addWidget(p.lblBrakeState);
        bl->addStretch();

        /* ------- SDO 区 ------- */
        auto *gbSdo = new QGroupBox("SDO 参数读写");
        auto *sl = new QFormLayout(gbSdo);
        p.edIdx = new QLineEdit("0x6091"); // 举例：齿轮比
        p.edSub = new QLineEdit("0x00");
        p.edVal = new QLineEdit("0");
        p.cbBits= new QComboBox; p.cbBits->addItems({"8","16","32"}); p.cbBits->setCurrentIndex(1);
        p.cbSign= new QComboBox; p.cbSign->addItems({"无符号","有符号"});
        p.btnSdoRead = new QPushButton("读取");
        p.btnSdoWrite= new QPushButton("写入");
        p.lblSdoResult = new QLabel("-"); p.lblSdoResult->setFont(mono);

        auto *sdoRow = new QHBoxLayout;
        sdoRow->addWidget(new QLabel("索引:"));    sdoRow->addWidget(p.edIdx);
        sdoRow->addWidget(new QLabel("子索引:"));  sdoRow->addWidget(p.edSub);
        sdoRow->addWidget(new QLabel("位宽:"));    sdoRow->addWidget(p.cbBits);
        sdoRow->addWidget(new QLabel("符号:"));    sdoRow->addWidget(p.cbSign);
        auto *sdoRowW = new QWidget; sdoRowW->setLayout(sdoRow);

        auto *sdoRow2 = new QHBoxLayout;
        sdoRow2->addWidget(new QLabel("值:"));     sdoRow2->addWidget(p.edVal);
        sdoRow2->addWidget(p.btnSdoRead); sdoRow2->addWidget(p.btnSdoWrite);
        auto *sdoRow2W = new QWidget; sdoRow2W->setLayout(sdoRow2);

        sl->addRow(sdoRowW);
        sl->addRow(sdoRow2W);
        sl->addRow("结果:", p.lblSdoResult);

        lay->addWidget(gbSt);
        lay->addWidget(gbCtl);
        lay->addWidget(gbBrake);
        lay->addWidget(gbSdo);
        lay->addStretch();

        /* 小分辨率下用滚动容器，避免被挤压 */
        auto *scroll = new QScrollArea;
        scroll->setWidget(w);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);

        tabs_->addTab(scroll, QString("电机 %1").arg(i));

        /* 若扫描结果表明该位置不是电机，则禁用所有控制并改标签 */
        bool isMotor = (i < motorMask_.size()) ? motorMask_[i] : true;
        if (!isMotor) {
            tabs_->setTabText(tabs_->count() - 1, QString("从站 %1 (非电机)").arg(i));
            /* 禁用全部控制按钮/SDO，避免误操作 */
            for (QWidget *ww : {(QWidget*)gbCtl, (QWidget*)gbBrake, (QWidget*)gbSdo}) {
                ww->setEnabled(false);
            }
            gbSt->setTitle(QString("从站 %1 (非电机设备)").arg(i));
        }

        /* ---- 信号连接（用 lambda 捕获电机序号） ---- */
        connect(p.btnEnable,     &QPushButton::clicked, this, [this,i]{ onEnable(i); });
        connect(p.btnDisable,    &QPushButton::clicked, this, [this,i]{ onDisable(i); });
        connect(p.btnFaultReset, &QPushButton::clicked, this, [this,i]{ onFaultReset(i); });
        connect(p.btnApply,      &QPushButton::clicked, this, [this,i]{ onApply(i); });
        connect(p.btnSdoRead,    &QPushButton::clicked, this, [this,i]{ onSdoRead(i); });
        connect(p.btnSdoWrite,   &QPushButton::clicked, this, [this,i]{ onSdoWrite(i); });
        connect(p.cbMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this,i](int){ onModeChanged(i); });
        connect(p.btnBrakeOpen,  &QPushButton::clicked, this, [this,i]{ onBrakeOpen(i); });
        connect(p.btnBrakeClose, &QPushButton::clicked, this, [this,i]{ onBrakeClose(i); });
        connect(p.btnBrakeQuery, &QPushButton::clicked, this, [this,i]{ onBrakeQuery(i); });
    }
}

/* ---------------------------------------------------------------- */
void MainWindow::appendLog(const QString &m) {
    log_->appendPlainText(QString("[%1] %2")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")).arg(m));
}

void MainWindow::onLog(const QString &m)   { appendLog(m); }
void MainWindow::onError(const QString &m) { appendLog("<错误> " + m);
                                             QMessageBox::warning(this, "EtherCAT", m); }

/* ---------------------------------------------------------------- */
void MainWindow::onScan() {
    if (running_) {
        QMessageBox::information(this, "扫描从站", "请先停止主站，再进行扫描");
        return;
    }
    appendLog("扫描从站 ...");
    btnScan_->setEnabled(false);
    QApplication::processEvents();

    ec_master_t *m = ecrt_request_master(0);
    if (!m) {
        appendLog("<错误> ecrt_request_master(0) 失败: 请检查 ethercat 内核模块是否加载 (systemctl status ethercat)");
        btnScan_->setEnabled(true);
        return;
    }

    ec_master_info_t mi;
    int r = ecrt_master(m, &mi);
    if (r) {
        appendLog(QString("<错误> ecrt_master 失败: %1").arg(r));
        ecrt_release_master(m);
        btnScan_->setEnabled(true);
        return;
    }

    int n = (int)mi.slave_count;
    appendLog(QString("扫描到 %1 个从站 (link_up=%2)").arg(n).arg(mi.link_up));

    uint32_t firstVendor = 0, firstProduct = 0;
    bool allSame = (n > 0);
    QVector<ec_slave_info_t> infos(n);
    for (int i = 0; i < n; ++i) {
        ec_slave_info_t si{};
        if (ecrt_master_get_slave(m, (uint16_t)i, &si) == 0) {
            infos[i] = si;
            const QString xml = si.revision_number == 143
                ? "EYOU_ServoModule_ECAT_V143.xml"
                : (si.revision_number == 145 ? "EYOU_ServoModule_ECAT_V145.xml" : "<不支持>");
            appendLog(QString("  #%1  vendor=0x%2 product=0x%3 revision=V%4 AL=0x%5 名称=\"%6\" XML=%7")
                .arg(i)
                .arg(si.vendor_id, 8, 16, QChar('0'))
                .arg(si.product_code, 8, 16, QChar('0'))
                .arg(si.revision_number)
                .arg(si.al_state, 2, 16, QChar('0'))
                .arg(QString::fromUtf8(si.name))
                .arg(xml));
            if (i == 0) { firstVendor = si.vendor_id; firstProduct = si.product_code; }
            else if (si.vendor_id != firstVendor || si.product_code != firstProduct) allSame = false;
        } else {
            appendLog(QString("  #%1  <读取失败>").arg(i));
            allSame = false;
        }
    }
    ecrt_release_master(m);

    if (n > 0) {
        /* 选定用于电机识别的 vendor/product：若总线全为同一型号则直接采用；
           否则保留用户原有 vendor/product，只根据匹配情况标记电机位置 */
        uint32_t motorVendor  = firstVendor;
        uint32_t motorProduct = firstProduct;
        if (!allSame) {
            bool ok1=false, ok2=false;
            uint32_t v = edVendor_->text().toUInt(&ok1, 0);
            uint32_t p = edProduct_->text().toUInt(&ok2, 0);
            if (ok1 && ok2) { motorVendor = v; motorProduct = p; }
        }
        /* 根据选定 vendor/product 构建电机掩码 */
        motorMask_.fill(false, n);
        int motorCnt = 0;
        for (int i = 0; i < n; ++i) {
            if (infos[i].vendor_id == motorVendor && infos[i].product_code == motorProduct) {
                motorMask_[i] = true; ++motorCnt;
            }
        }
        appendLog(QString("识别到电机从站 %1 个 (vendor=0x%2 product=0x%3)")
            .arg(motorCnt)
            .arg(motorVendor, 8, 16, QChar('0'))
            .arg(motorProduct, 8, 16, QChar('0')));

        if (allSame) {
            edVendor_->setText(QString("0x%1").arg(firstVendor, 8, 16, QChar('0')));
            edProduct_->setText(QString("0x%1").arg(firstProduct, 8, 16, QChar('0')));
            appendLog(QString("自动填充 Vendor/Product: 0x%1 / 0x%2")
                .arg(firstVendor, 8, 16, QChar('0'))
                .arg(firstProduct, 8, 16, QChar('0')));
        } else {
            appendLog("总线存在混合设备；仅对匹配当前 Vendor/Product 的位置执行电机控制，其它位置将仅显示 AL 状态。");
        }

        /* 如果 setValue 因为值未变化不会发出信号，这里显式刷新一次面板 */
        const bool valueChanged = (spSlaves_->value() != n);
        spSlaves_->setValue(n);
        if (!valueChanged) rebuildMotorTabs(n);
    } else {
        motorMask_.clear();
        appendLog("未发现从站。请检查网线连接、供电、以及主站绑定的网卡。");
    }
    btnScan_->setEnabled(true);
}

/* ---------------------------------------------------------------- */
void MainWindow::onStart() {
    if (running_) return;
    bool ok1=false, ok2=false;
    uint32_t vendor  = edVendor_->text().toUInt(&ok1, 0);
    uint32_t product = edProduct_->text().toUInt(&ok2, 0);
    if (!ok1 || !ok2) { QMessageBox::warning(this,"参数","Vendor/Product 格式错误(支持 0x...)"); return; }

    EcConfig cfg;
    cfg.slaveCount = spSlaves_->value();
    cfg.vendor     = vendor;
    cfg.product    = product;
    cfg.cycleUs    = spCycleUs_->value();
    /* 若已扫描得到混合设备的电机掩码，长度吻合则传给 worker */
    if (motorMask_.size() == cfg.slaveCount) cfg.motorMask = motorMask_;

    if (worker_) { worker_->requestStop(); worker_->wait(); delete worker_; worker_=nullptr; }
    worker_ = new EcWorker(this);
    worker_->configure(cfg);

    connect(worker_, &EcWorker::logMessage,    this, &MainWindow::onLog);
    connect(worker_, &EcWorker::errorOccurred, this, &MainWindow::onError);
    connect(worker_, &EcWorker::masterStarted, this, &MainWindow::onMasterStarted);
    connect(worker_, &EcWorker::masterStopped, this, &MainWindow::onMasterStopped);
    connect(worker_, &EcWorker::sdoFinished,   this, &MainWindow::onSdoResult);
    connect(worker_, &EcWorker::motionRecordFinished, this, &MainWindow::onMotionRecorded);
    connect(worker_, &EcWorker::motionStateChanged, this, &MainWindow::onMotionState);

    appendLog(QString("启动 EtherCAT：%1 从站, 周期 %2us").arg(cfg.slaveCount).arg(cfg.cycleUs));
    btnStart_->setEnabled(false);
    btnScan_->setEnabled(false);
    spSlaves_->setEnabled(false);
    edVendor_->setEnabled(false);
    edProduct_->setEnabled(false);
    spCycleUs_->setEnabled(false);
    lblMasterState_->setText("● 启动中..."); lblMasterState_->setStyleSheet("color:orange;font-weight:bold;");
    worker_->start(QThread::TimeCriticalPriority);
}

void MainWindow::onStop() {
    if (!worker_) return;
    btnStop_->setEnabled(false);     // 防重入
    appendLog("停止主站...");
    worker_->requestStop();
}

void MainWindow::onMasterStarted() {
    running_ = true;
    for (int i = 0; i < panels_.size(); ++i) {
        MotorCommand c = buildCmdFromPanel(panels_[i]);
        const bool isMotor = i >= motorMask_.size() || motorMask_[i];
        c.enable = isMotor; // 启动后自动使能所有已识别电机
        c.hasTarget = false;
        worker_->setCommand(i, c);
        if (isMotor) {
            panels_[i].btnEnable->setProperty("on", true);
            panels_[i].btnEnable->setStyleSheet("background:#8f8;font-weight:bold;");
        }
    }
    btnStop_->setEnabled(true);
    btnSdoSafe_->setEnabled(true);
    lblMasterState_->setText("● 运行中");
    lblMasterState_->setStyleSheet("color:green;font-weight:bold;");
    appendLog("主站已启动：已自动请求使能全部 EU 电机（CSP 当前位置保持）");
    timer_->start();
}

void MainWindow::onMasterStopped() {
    running_ = false;
    recording_ = false;
    recordPreparing_ = false;
    pendingRecordBrakes_ = 0;
    playbackPreparing_ = false;
    playbackActive_ = false;
    pendingPlaybackBrakes_ = 0;
    timer_->stop();
    btnStart_->setEnabled(true);
    btnStop_->setEnabled(false);
    btnScan_->setEnabled(true);
    btnSdoSafe_->setEnabled(false);
    btnSdoSafe_->setChecked(false);
    spSlaves_->setEnabled(true);
    edVendor_->setEnabled(true);
    edProduct_->setEnabled(true);
    spCycleUs_->setEnabled(true);
    lblMasterState_->setText("● 已停止");
    lblMasterState_->setStyleSheet("color:gray;font-weight:bold;");
    if (lblMotionState_) lblMotionState_->setText("主站已停止");
    if (btnRecord_) { btnRecord_->setText("开始录制（失能）"); btnRecord_->setEnabled(true); }
    if (btnPlay_) btnPlay_->setEnabled(true);
    if (tabs_) tabs_->setEnabled(true);
    appendLog("主站已停止");
}

/* ================= 机械臂动作库 ================= */
static QString motionLibraryPath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + "/robot_motions.json";
}

void MainWindow::loadMotionLibrary() {
    motions_.clear();
    QFile f(motionLibraryPath());
    if (!f.exists()) { refreshMotionList(); return; }
    if (!f.open(QIODevice::ReadOnly)) { appendLog("<警告> 无法读取动作库: " + f.errorString()); return; }
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        appendLog("<警告> 动作库 JSON 损坏: " + pe.errorString()); return;
    }
    for (const QJsonValue &v : doc.object().value("motions").toArray()) {
        QJsonObject o = v.toObject(); SavedMotion m;
        m.name = o.value("name").toString().trimmed();
        m.created = o.value("created").toString();
        m.data.sampleMs = o.value("sample_ms").toInt(20);
        int axes = o.value("axes").toInt();
        for (const QJsonValue &fv : o.value("frames").toArray()) {
            QVector<int32_t> frame;
            for (const QJsonValue &p : fv.toArray()) frame.push_back((int32_t)p.toDouble());
            if (axes > 0 && frame.size() == axes) m.data.frames.push_back(frame);
        }
        if (!m.name.isEmpty() && m.data.frames.size() >= 2) motions_.push_back(m);
    }
    refreshMotionList();
    appendLog(QString("已加载 %1 条机械臂动作: %2").arg(motions_.size()).arg(motionLibraryPath()));
}

bool MainWindow::saveMotionLibrary() {
    QJsonArray list;
    for (const SavedMotion &m : motions_) {
        QJsonObject o; o["name"] = m.name; o["created"] = m.created;
        o["sample_ms"] = m.data.sampleMs;
        o["axes"] = m.data.frames.isEmpty() ? 0 : m.data.frames.first().size();
        QJsonArray frames;
        for (const auto &f : m.data.frames) {
            QJsonArray a; for (int32_t p : f) a.append((double)p); frames.append(a);
        }
        o["frames"] = frames; list.append(o);
    }
    QJsonObject root; root["version"] = 1; root["motions"] = list;
    QSaveFile f(motionLibraryPath());
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(root).toJson()) < 0 || !f.commit()) {
        QMessageBox::warning(this, "动作库", "保存失败: " + f.errorString()); return false;
    }
    return true;
}

void MainWindow::refreshMotionList() {
    if (!motionList_) return;
    int row = motionList_->currentRow(); motionList_->clear();
    for (const SavedMotion &m : motions_) {
        double seconds = (m.data.frames.size() - 1) * m.data.sampleMs / 1000.0;
        motionList_->addItem(QString("%1  |  %2 帧 / %3 s")
            .arg(m.name).arg(m.data.frames.size()).arg(seconds, 0, 'f', 2));
    }
    if (!motions_.isEmpty()) motionList_->setCurrentRow(qBound(0, row, motions_.size() - 1));
}

void MainWindow::onRecordToggle() {
    if (!worker_ || !running_) { QMessageBox::information(this, "动作录制", "请先启动 EtherCAT 主站"); return; }
    if (!recording_ && !recordPreparing_) {
        recordPreparing_ = true;
        pendingRecordSampleMs_ = spRecordMs_->value();
        pendingRecordBrakes_ = 0;
        failedRecordBrakes_ = 0;
        btnRecord_->setEnabled(false);
        tabs_->setEnabled(false); btnPlay_->setEnabled(false); btnReturn_->setEnabled(false); btnReturnPlay_->setEnabled(false);
        btnEnableAll_->setEnabled(false); btnFaultResetAll_->setEnabled(false);
        btnDisableRelease_->setEnabled(false); btnSdoSafe_->setEnabled(false);
        lblMotionState_->setText("正在失能并松开抱闸...");
        setAllMotorCommandsDisabled();
        appendLog("准备录制：先失能所有电机，再自动松开所有 EU 电机抱闸");
        /* 给 CiA402 状态机若干周期完成失能，再通过 SDO 操作机械抱闸。 */
        QTimer::singleShot(100, this, [this] {
            if (recordPreparing_ && worker_ && running_) writeAllBrakes(true, "WRECBRK");
        });
    } else worker_->endMotionRecord();
}

void MainWindow::onMotionRecorded(RecordedMotion motion) {
    recording_ = false; btnRecord_->setText("开始录制（失能）");
    tabs_->setEnabled(true); btnPlay_->setEnabled(true); btnReturn_->setEnabled(true); btnReturnPlay_->setEnabled(true);
    btnEnableAll_->setEnabled(true); btnFaultResetAll_->setEnabled(true);
    btnDisableRelease_->setEnabled(true); btnSdoSafe_->setEnabled(running_);
    writeAllBrakes(false, "WRECCLOSE");
    appendLog("录制结束：已请求抱死所有电机抱闸");
    if (motion.frames.size() < 2) { QMessageBox::warning(this, "动作录制", "有效采样不足 2 帧，未保存"); return; }
    bool ok = false;
    QString name = QInputDialog::getText(this, "保存动作", "动作名称:", QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) { appendLog("动作录制已取消保存"); return; }
    int existing = -1;
    for (int i = 0; i < motions_.size(); ++i) if (motions_[i].name == name) { existing = i; break; }
    if (existing >= 0 && QMessageBox::question(this, "覆盖动作", "名称已存在，是否覆盖？") != QMessageBox::Yes) return;
    SavedMotion m{name, QDateTime::currentDateTime().toString(Qt::ISODate), motion};
    if (existing >= 0) motions_[existing] = m; else motions_.push_back(m);
    if (saveMotionLibrary()) {
        refreshMotionList(); motionList_->setCurrentRow(existing >= 0 ? existing : motions_.size() - 1);
        appendLog(QString("动作“%1”已保存：%2 帧，%3 ms/帧").arg(name).arg(motion.frames.size()).arg(motion.sampleMs));
    }
}

void MainWindow::onEnableAllMotion() {
    if (!worker_ || !running_) { QMessageBox::information(this, "机械臂使能", "请先启动 EtherCAT 主站"); return; }
    QVector<MotorStatus> states = worker_->snapshot();
    for (int i = 0; i < panels_.size(); ++i) {
        bool motor = i < states.size() ? states[i].isMotor
                                       : (i >= motorMask_.size() || motorMask_[i]);
        if (motor) onEnable(i);
    }
    lblMotionState_->setText("已请求全部使能");
    appendLog("机械臂：已向所有 EU 电机发送使能请求");
}

void MainWindow::onFaultResetAllMotion() {
    if (!worker_ || !running_) { QMessageBox::information(this, "一键故障复位", "请先启动 EtherCAT 主站"); return; }
    QVector<MotorStatus> states = worker_->snapshot();
    int count = 0;
    for (int i = 0; i < panels_.size(); ++i) {
        bool motor = i < states.size() ? states[i].isMotor
                                       : (i >= motorMask_.size() || motorMask_[i]);
        if (!motor) continue;
        MotorCommand c;
        c.opMode = MODE_CSP;
        c.enable = false;       // 复位时不自动重新使能，防止机械臂意外运动
        c.hasTarget = false;
        c.faultReset = true;    // EcWorker 消费后自动清零，形成单次 0x0080 脉冲
        worker_->setCommand(i, c);
        panels_[i].btnEnable->setProperty("on", false);
        ++count;
    }
    lblMotionState_->setText("已发送一键故障复位");
    appendLog(QString("一键故障复位：已向 %1 个 EU 电机发送 Controlword 0x0080 脉冲").arg(count));
}

void MainWindow::onDisableReleaseAll() {
    if (!worker_ || !running_) { QMessageBox::information(this, "机械臂失能", "请先启动 EtherCAT 主站"); return; }
    playbackPreparing_ = false;
    worker_->stopMotionActivity();
    setAllMotorCommandsDisabled();
    lblMotionState_->setText("正在失能并松开抱闸...");
    QTimer::singleShot(100, this, [this] {
        writeAllBrakes(true, "WFREEBRK");
        lblMotionState_->setText("已失能并请求松开抱闸");
        appendLog("机械臂：所有电机已失能，并请求松开全部抱闸");
    });
}

void MainWindow::onReturnMotion() {
    autoPlayAfterReturn_ = false;
    int row = motionList_->currentRow();
    if (!worker_ || !running_) { QMessageBox::information(this, "动作播放", "请先启动 EtherCAT 主站"); return; }
    if (worker_->sdoSafeMode()) { QMessageBox::information(this, "动作播放", "请先退出 SDO 调参模式"); return; }
    if (row < 0 || row >= motions_.size()) { QMessageBox::information(this, "动作播放", "请选择一条动作"); return; }
    const RecordedMotion &m = motions_[row].data;
    if (m.frames.first().size() != spSlaves_->value()) { QMessageBox::warning(this, "动作播放", "动作轴数与当前从站数不匹配"); return; }
    QVector<MotorStatus> snap = worker_->snapshot();
    for (const MotorStatus &s : snap) if (s.isMotor && ((s.statusWord & (1 << 3)) || s.errorCode)) {
        QMessageBox::warning(this, "动作播放", "存在电机故障，请复位后再播放"); return;
    }
    pendingPlaybackMotion_ = m;
    pendingPlaybackSpeed_ = spReturnSpeed_->value();
    pendingPlaybackName_ = motions_[row].name;
    playbackPreparing_ = true; failedPlaybackBrakes_ = 0; pendingPlaybackBrakes_ = 0;
    tabs_->setEnabled(false); btnRecord_->setEnabled(false); btnSdoSafe_->setEnabled(false);
    btnEnableAll_->setEnabled(false); btnFaultResetAll_->setEnabled(false);
    btnDisableRelease_->setEnabled(false); btnReturn_->setEnabled(false); btnReturnPlay_->setEnabled(false);
    btnPlay_->setEnabled(false);

    bool anyMotor = false;
    bool allMotorEnabled = true;
    for (const MotorStatus &s : snap) {
        if (!s.isMotor) continue;
        anyMotor = true;
        const uint16_t st = (uint16_t)(s.statusWord & 0x6F);
        if (st != 0x27) {  // CiA402 OperationEnabled
            allMotorEnabled = false;
            break;
        }
    }
    if (anyMotor && allMotorEnabled) {
        playbackPreparing_ = false;
        if (!worker_->beginMotionReturnToStart(pendingPlaybackMotion_, pendingPlaybackSpeed_, true)) {
            tabs_->setEnabled(true); btnRecord_->setEnabled(true); btnPlay_->setEnabled(true);
            btnReturn_->setEnabled(true); btnReturnPlay_->setEnabled(true);
            btnEnableAll_->setEnabled(true); btnFaultResetAll_->setEnabled(true); btnDisableRelease_->setEnabled(true);
            btnSdoSafe_->setEnabled(running_);
            QMessageBox::warning(this, "回起点", "当前已使能，但回起点任务启动失败");
            return;
        }
        playbackActive_ = true;
        lblMotionState_->setText("回到动作起点");
        appendLog(QString("回起点：检测到动作轴已处于使能状态，跳过失能/松闸，直接回到动作“%1”起点")
            .arg(pendingPlaybackName_));
        return;
    }

    lblMotionState_->setText("正在松开抱闸...");
    setAllMotorCommandsDisabled();
    appendLog(QString("准备回到动作“%1”的起点：失能后自动松开全部抱闸").arg(pendingPlaybackName_));
    QTimer::singleShot(100, this, [this] {
        if (playbackPreparing_ && worker_ && running_) writeAllBrakes(true, "WPLAYBRK");
    });
}

void MainWindow::onReturnAndPlayMotion() {
    onReturnMotion();
    /* 回起点可能处于松闸准备中，或已使能时直接开始回起点；两种情况均需自动续播。 */
    if (playbackPreparing_ || playbackActive_) {
        autoPlayAfterReturn_ = true;
        appendLog("已选择快捷流程：到达起点后自动播放");
    }
}

void MainWindow::onPlayMotion() {
    if (!worker_ || !running_) { QMessageBox::information(this, "动作播放", "请先启动 EtherCAT 主站"); return; }
    if (!worker_->startPreparedMotionPlayback()) {
        QMessageBox::information(this, "动作播放", "请先选择动作并点击“回起点”，等待状态显示“已到起点”后再播放");
        return;
    }
    playbackActive_ = true;
    btnReturn_->setEnabled(false);
    btnReturnPlay_->setEnabled(false);
    appendLog("从当前动作起点开始播放轨迹");
}

void MainWindow::onStopMotion() {
    autoPlayAfterReturn_ = false;
    playbackPreparing_ = false;
    if (worker_) worker_->stopMotionActivity();
    if (playbackActive_ || pendingPlaybackBrakes_ > 0)
        QTimer::singleShot(100, this, [this]{ writeAllBrakes(false, "WPLAYCLOSE"); });
    playbackActive_ = false;
}

void MainWindow::onDeleteMotion() {
    int row = motionList_->currentRow(); if (row < 0 || row >= motions_.size()) return;
    if (QMessageBox::question(this, "删除动作", "确定删除“" + motions_[row].name + "”？") != QMessageBox::Yes) return;
    motions_.removeAt(row); if (saveMotionLibrary()) refreshMotionList();
}

void MainWindow::onMotionState(const QString &state) {
    lblMotionState_->setText(state); appendLog("机械臂动作状态: " + state);
    if (state == "已到起点") {
        btnPlay_->setEnabled(true);
        btnMotionStop_->setEnabled(true);
        if (autoPlayAfterReturn_) {
            autoPlayAfterReturn_ = false;
            QTimer::singleShot(0, this, &MainWindow::onPlayMotion);
        }
    }
    if (state == "空闲" || state == "播放完成") {
        if (state == "播放完成" && playbackActive_) {
            appendLog("动作播放完成：保持使能，不自动抱闸");
            playbackActive_ = false;
        }
        tabs_->setEnabled(true); btnRecord_->setEnabled(true); btnPlay_->setEnabled(true);
        btnReturn_->setEnabled(true); btnReturnPlay_->setEnabled(true);
        btnEnableAll_->setEnabled(true); btnFaultResetAll_->setEnabled(true); btnDisableRelease_->setEnabled(true);
        btnSdoSafe_->setEnabled(running_);
    }
}

void MainWindow::setAllMotorCommandsDisabled() {
    if (!worker_) return;
    QVector<MotorStatus> states = worker_->snapshot();
    for (int i = 0; i < panels_.size(); ++i) {
        bool motor = i < states.size() ? states[i].isMotor
                                       : (i >= motorMask_.size() || motorMask_[i]);
        if (motor) onDisable(i);
    }
}

void MainWindow::writeAllBrakes(bool open, const QString &tagPrefix) {
    if (!worker_ || !running_) return;
    QVector<MotorStatus> states = worker_->snapshot();
    int count = 0;
    for (int i = 0; i < panels_.size(); ++i) {
        bool motor = i < states.size() ? states[i].isMotor
                                       : (i >= motorMask_.size() || motorMask_[i]);
        if (!motor) continue;
        SdoJob j; j.slave = i; j.index = 0x2014; j.subindex = 1;
        j.bits = 8; j.isSigned = false; j.write = true; j.value = open ? 1 : 0;
        j.tag = QString("%1/M%2/%3").arg(tagPrefix).arg(i).arg(open ? "open" : "close");
        worker_->postSdo(j); ++count;
    }
    if (tagPrefix == "WRECBRK") {
        pendingRecordBrakes_ = count;
        if (count == 0) {
            recordPreparing_ = false;
            btnRecord_->setEnabled(true);
            tabs_->setEnabled(true); btnPlay_->setEnabled(true); btnSdoSafe_->setEnabled(running_);
            QMessageBox::warning(this, "动作录制", "没有识别到可松闸的 EU 电机");
        }
    } else if (tagPrefix == "WPLAYBRK") {
        pendingPlaybackBrakes_ = count;
        if (count == 0) {
            playbackPreparing_ = false;
            tabs_->setEnabled(true); btnRecord_->setEnabled(true); btnPlay_->setEnabled(true);
            btnSdoSafe_->setEnabled(running_);
            QMessageBox::warning(this, "动作播放", "没有识别到可松闸的 EU 电机");
        }
    }
}

void MainWindow::onToggleSdoSafe(bool on) {
    if (!worker_) return;
    worker_->setSdoSafeMode(on);
    if (on) {
        btnSdoSafe_->setText("运行模式(正在调参)");
        btnSdoSafe_->setStyleSheet("background:#f39c12;color:white;font-weight:bold;");
        lblMasterState_->setText("● 调参模式(已下电)");
        lblMasterState_->setStyleSheet("color:#d35400;font-weight:bold;");
        /* 复位所有"使能"按钮的本地状态，避免切回后直接按 apply 导致惊跳 */
        for (auto &p : panels_) p.btnEnable->setProperty("on", false);
        appendLog("进入 SDO 调参模式：所有电机已下电，可安全读写 SDO");
    } else {
        btnSdoSafe_->setText("SDO 调参模式(下电)");
        btnSdoSafe_->setStyleSheet("");
        lblMasterState_->setText("● 运行中");
        lblMasterState_->setStyleSheet("color:green;font-weight:bold;");
        appendLog("退出 SDO 调参模式：恢复 PDO 控制");
    }
}

/* ---------------------------------------------------------------- */
void MainWindow::onRefreshStatus() {
    if (!worker_) return;
    auto snap = worker_->snapshot();
    for (int i = 0; i < snap.size() && i < panels_.size(); ++i) {
        const MotorStatus &s = snap[i];
        MotorPanel &p = panels_[i];
        p.lblAl->setText(QString("%1  %2").arg(alStateText(s.alState)).arg(s.online?"online":"offline"));
        if (!s.isMotor) {
            p.lblSw->setText(QString("非电机设备  vendor=0x%1 product=0x%2")
                .arg(s.vendorId, 8, 16, QChar('0'))
                .arg(s.productCode, 8, 16, QChar('0')));
            p.lblPos->setText("-");
            p.lblVel->setText("-");
            p.lblTor->setText("-");
            p.lblErr->setText("-");
            p.lblModeDisp->setText("-");
            continue;
        }
        p.lblSw->setText(QString("0x%1  (%2)").arg(s.statusWord,4,16,QChar('0')).arg(swText(s.statusWord)));
        p.lblPos->setText(QString::number(s.actualPos));
        p.lblVel->setText(QString::number(s.actualVel));
        p.lblTor->setText(QString::number(s.actualTor));
        p.lblErr->setText(QString("0x%1").arg(s.errorCode,4,16,QChar('0')));
        p.lblModeDisp->setText(QString::number(s.opModeDisp));
    }
}

/* ---------------- 控制回调 ---------------- */
static MotorCommand buildCmdFromPanel(const MotorPanel &p) {
    MotorCommand c;
    c.opMode    = (int8_t)p.cbMode->currentData().toInt();
    c.absolute  = p.chkAbs->isChecked();
    c.jogStep   = p.spJogStep->value();
    int tgt = p.spTarget->value();
    if (c.opMode == MODE_CSP || c.opMode == MODE_PP || c.opMode == MODE_IP) c.targetPos = tgt;
    else if (c.opMode == MODE_CSV || c.opMode == MODE_PV) c.targetVel = tgt;
    else if (c.opMode == MODE_CST || c.opMode == MODE_PT) c.targetTor = (int16_t)tgt;
    return c;
}

void MainWindow::onModeChanged(int motor) {
    // 模式变化只更新本地显示（下次 Apply 才生效）
    Q_UNUSED(motor);
}

void MainWindow::onApply(int motor) {
    if (!worker_) return;
    MotorCommand c = buildCmdFromPanel(panels_[motor]);
    /* 保留当前 enable 状态：读取旧 command 值不方便，这里简单默认不改 enable */
    c.enable = (panels_[motor].btnEnable->property("on").toBool());
    c.hasTarget = true;   // 明确请求移动到新目标
    /* 每次点击自增 applySeq，PP/PV/HM 模式下 worker 据此打 new_setpoint 脉冲 */
    uint32_t seq = panels_[motor].btnApply->property("seq").toUInt() + 1;
    panels_[motor].btnApply->setProperty("seq", seq);
    c.applySeq = seq;
    worker_->setCommand(motor, c);
    if (c.opMode == MODE_CSP) {
        appendLog(QString("电机 %1 应用新目标值 (CSP 相对偏移=%2, 每周期步长=%3)")
            .arg(motor).arg(c.targetPos).arg(c.jogStep));
    } else if (c.opMode == MODE_CSV) {
        appendLog(QString("电机 %1 应用新目标值 (CSV 目标速度=%2)").arg(motor).arg(c.targetVel));
    } else if (c.opMode == MODE_CST) {
        appendLog(QString("电机 %1 应用新目标值 (CST 目标力矩=%2)").arg(motor).arg(c.targetTor));
    } else if (c.opMode == MODE_PP) {
        /* PP 模式：先把 0x6081/6083/6084 通过 SDO 写下去(仅当数值变化或首次 apply) */
        MotorPanel &pnl = panels_[motor];
        int vel = pnl.spProfVel->value();
        int acc = pnl.spProfAcc->value();
        int dec = pnl.spProfDec->value();
        auto postU32 = [&](uint16_t idx, uint8_t sub, int val, const char *what) {
            SdoJob j; j.slave = motor; j.index = idx; j.subindex = sub;
            j.bits = 32; j.isSigned = false; j.write = true; j.value = val;
            j.tag = QString("W/M%1/0x%2:%3").arg(motor).arg(idx, 4, 16, QChar('0')).arg(sub);
            worker_->postSdo(j);
            appendLog(QString("  SDO 写 0x%1:%2 (%3) = %4")
                .arg(idx, 4, 16, QChar('0')).arg(sub).arg(what).arg(val));
        };
        if (pnl.btnApply->property("lastVel").toInt() != vel) {
            postU32(0x6081, 0, vel, "profile velocity"); pnl.btnApply->setProperty("lastVel", vel);
        }
        if (pnl.btnApply->property("lastAcc").toInt() != acc) {
            postU32(0x6083, 0, acc, "profile acceleration"); pnl.btnApply->setProperty("lastAcc", acc);
        }
        if (pnl.btnApply->property("lastDec").toInt() != dec) {
            postU32(0x6084, 0, dec, "profile deceleration"); pnl.btnApply->setProperty("lastDec", dec);
        }
        appendLog(QString("电机 %1 应用新目标值 (PP 目标位置=%2 %3 seq=%4, vel=%5)")
            .arg(motor).arg(c.targetPos).arg(c.absolute ? "绝对" : "相对").arg(seq).arg(vel));
    } else if (c.opMode == MODE_PV) {
        appendLog(QString("电机 %1 应用新目标值 (PV 目标速度=%2 seq=%3)")
            .arg(motor).arg(c.targetVel).arg(seq));
    } else {
        appendLog(QString("电机 %1 应用新目标值 (mode=%2)").arg(motor).arg((int)c.opMode));
    }

    /* ---- PT / HM / IP 模式的关键 SDO 参数下发 ---- */
    auto postU32s = [&](uint16_t idx, uint8_t sub, int val, bool signedInt, const char *what) {
        SdoJob j; j.slave = motor; j.index = idx; j.subindex = sub;
        j.bits = 32; j.isSigned = signedInt; j.write = true; j.value = val;
        j.tag = QString("W/M%1/0x%2:%3").arg(motor).arg(idx, 4, 16, QChar('0')).arg(sub);
        worker_->postSdo(j);
        appendLog(QString("  SDO 写 0x%1:%2 (%3) = %4")
            .arg(idx, 4, 16, QChar('0')).arg(sub).arg(what).arg(val));
    };
    auto postI8 = [&](uint16_t idx, uint8_t sub, int val, const char *what) {
        SdoJob j; j.slave = motor; j.index = idx; j.subindex = sub;
        j.bits = 8; j.isSigned = true; j.write = true; j.value = val;
        j.tag = QString("W/M%1/0x%2:%3").arg(motor).arg(idx, 4, 16, QChar('0')).arg(sub);
        worker_->postSdo(j);
        appendLog(QString("  SDO 写 0x%1:%2 (%3) = %4")
            .arg(idx, 4, 16, QChar('0')).arg(sub).arg(what).arg(val));
    };
    MotorPanel &pnl = panels_[motor];
    auto changed = [&](const char *key, int v){
        if (pnl.btnApply->property(key).toInt() == v) return false;
        pnl.btnApply->setProperty(key, v); return true;
    };
    if (c.opMode == MODE_PT) {
        int slope = pnl.spTorSlope->value();
        if (changed("lastTorSlope", slope)) postU32s(0x6087, 0, slope, false, "torque slope");
    } else if (c.opMode == MODE_HM) {
        int m = pnl.spHomeMethod->value();
        int s1 = pnl.spHomeSpdSw->value();
        int s2 = pnl.spHomeSpdZero->value();
        int ha = pnl.spHomeAcc->value();
        int ho = pnl.spHomeOffset->value();
        if (changed("lastHomeMethod", m)) postI8(0x6098, 0, m, "homing method");
        if (changed("lastHomeSpdSw", s1)) postU32s(0x6099, 1, s1, false, "homing speed (search switch)");
        if (changed("lastHomeSpdZero",s2)) postU32s(0x6099, 2, s2, false, "homing speed (search zero)");
        if (changed("lastHomeAcc",   ha)) postU32s(0x609A, 0, ha, false, "homing acceleration");
        if (changed("lastHomeOffset",ho)) postU32s(0x607C, 0, ho, true,  "home offset");
    } else if (c.opMode == MODE_IP) {
        int ms = pnl.spIpTimeMs->value();
        if (changed("lastIpMs", ms)) {
            postU32s(0x60C2, 1, ms, false, "interpolation time units");
            /* 0x60C2:02 时间索引：-3 表示 10^-3(毫秒)，手册默认即 ms。用 SDO 写 i8=-3。 */
            SdoJob j; j.slave = motor; j.index = 0x60C2; j.subindex = 2;
            j.bits = 8; j.isSigned = true; j.write = true; j.value = -3;
            j.tag = QString("W/M%1/0x60C2:2").arg(motor);
            worker_->postSdo(j);
            appendLog("  SDO 写 0x60C2:2 (interpolation time index) = -3");
        }
    }
}

void MainWindow::onEnable(int motor) {
    if (!worker_) return;
    MotorCommand c = buildCmdFromPanel(panels_[motor]);
    c.enable = true;
    c.hasTarget = false;  // 先使能保持原位，待用户点击"应用目标值"再运动
    panels_[motor].btnEnable->setProperty("on", true);
    worker_->setCommand(motor, c);
    appendLog(QString("电机 %1 请求使能").arg(motor));
}

void MainWindow::onDisable(int motor) {
    if (!worker_) return;
    MotorCommand c = buildCmdFromPanel(panels_[motor]);
    c.enable = false;
    c.hasTarget = false;
    panels_[motor].btnEnable->setProperty("on", false);
    worker_->setCommand(motor, c);
    appendLog(QString("电机 %1 请求关闭").arg(motor));
}

void MainWindow::onFaultReset(int motor) {
    if (!worker_) return;
    MotorCommand c = buildCmdFromPanel(panels_[motor]);
    c.enable = panels_[motor].btnEnable->property("on").toBool();
    c.hasTarget = false;
    c.faultReset = true;
    worker_->setCommand(motor, c);
    appendLog(QString("电机 %1 故障复位").arg(motor));
}

/* ---------------- SDO ---------------- */
void MainWindow::onSdoRead(int motor) {
    if (!worker_) { QMessageBox::information(this,"提示","请先启动主站"); return; }
    MotorPanel &p = panels_[motor];
    bool a=false,b=false;
    SdoJob j;
    j.slave = motor;
    j.index = (uint16_t)p.edIdx->text().toUInt(&a, 0);
    j.subindex = (uint8_t)p.edSub->text().toUInt(&b, 0);
    j.bits  = p.cbBits->currentText().toInt();
    j.isSigned = (p.cbSign->currentIndex() == 1);
    j.write = false;
    j.tag = QString("R/M%1/0x%2:%3").arg(motor).arg(j.index,4,16,QChar('0')).arg(j.subindex);
    if (!a || !b) { QMessageBox::warning(this,"SDO","索引/子索引格式错误"); return; }
    worker_->postSdo(j);
    p.lblSdoResult->setText("读取中...");
}

void MainWindow::onSdoWrite(int motor) {
    if (!worker_) { QMessageBox::information(this,"提示","请先启动主站"); return; }
    MotorPanel &p = panels_[motor];
    bool a=false,b=false,c=false;
    SdoJob j;
    j.slave = motor;
    j.index = (uint16_t)p.edIdx->text().toUInt(&a, 0);
    j.subindex = (uint8_t)p.edSub->text().toUInt(&b, 0);
    j.bits  = p.cbBits->currentText().toInt();
    j.isSigned = (p.cbSign->currentIndex() == 1);
    j.write = true;
    j.value = p.edVal->text().toLongLong(&c, 0);
    j.tag = QString("W/M%1/0x%2:%3").arg(motor).arg(j.index,4,16,QChar('0')).arg(j.subindex);
    if (!a || !b || !c) { QMessageBox::warning(this,"SDO","索引/子索引/值格式错误"); return; }
    worker_->postSdo(j);
    p.lblSdoResult->setText("写入中...");
}

void MainWindow::onSdoResult(SdoResult r) {
    /* 解析 tag 取出 motor 序号 */
    int motor = -1;
    int m1 = r.tag.indexOf("/M"); int m2 = r.tag.indexOf("/", m1+2);
    if (m1>=0 && m2>m1) motor = r.tag.mid(m1+2, m2-m1-2).toInt();
    QString txt = r.ok
        ? (r.tag.startsWith("R") ? QString("OK 读到: %1 (0x%2)").arg(r.value).arg(r.value,0,16)
                                 : QString("OK 写入完成: %1").arg(r.value))
        : QString("失败: %1").arg(r.err);
    appendLog(QString("[SDO %1] %2").arg(r.tag).arg(txt));

    /* 录制准备流程：只有全部电机确认松闸写入成功后才开始位置采样。 */
    if (r.tag.startsWith("WRECBRK/")) {
        if (!r.ok) ++failedRecordBrakes_;
        if (pendingRecordBrakes_ > 0) --pendingRecordBrakes_;
        if (pendingRecordBrakes_ == 0 && recordPreparing_) {
            recordPreparing_ = false;
            if (failedRecordBrakes_ > 0) {
                writeAllBrakes(false, "WRECCLOSE");
                btnRecord_->setEnabled(true);
                tabs_->setEnabled(true); btnPlay_->setEnabled(true); btnSdoSafe_->setEnabled(running_);
                lblMotionState_->setText("松开抱闸失败");
                QMessageBox::warning(this, "动作录制",
                    QString("有 %1 个电机松开抱闸失败，已取消录制并重新抱闸。请检查 0x2014 对象和驱动器状态。")
                        .arg(failedRecordBrakes_));
            } else if (!worker_->beginMotionRecord(pendingRecordSampleMs_)) {
                writeAllBrakes(false, "WRECCLOSE");
                btnRecord_->setEnabled(true);
                tabs_->setEnabled(true); btnPlay_->setEnabled(true); btnSdoSafe_->setEnabled(running_);
                QMessageBox::warning(this, "动作录制", "松闸成功，但无法开始录制：已有其他动作任务");
            } else {
                recording_ = true;
                btnRecord_->setText("停止录制并保存"); btnRecord_->setEnabled(true);
                appendLog(QString("全部电机抱闸已松开，开始动作录制，采样周期 %1 ms")
                    .arg(pendingRecordSampleMs_));
                /* 读取实际抱闸状态用于面板回显，不作为写成功后的额外阻塞条件。 */
                for (int i = 0; i < panels_.size(); ++i) onBrakeQuery(i);
            }
        }
    }

    /* 播放准备流程：播放前也必须先松开所有轴抱闸。 */
    if (r.tag.startsWith("WPLAYBRK/")) {
        if (!r.ok) ++failedPlaybackBrakes_;
        if (pendingPlaybackBrakes_ > 0) --pendingPlaybackBrakes_;
        if (pendingPlaybackBrakes_ == 0 && playbackPreparing_) {
            playbackPreparing_ = false;
            if (failedPlaybackBrakes_ > 0) {
                writeAllBrakes(false, "WPLAYCLOSE");
                tabs_->setEnabled(true); btnRecord_->setEnabled(true); btnPlay_->setEnabled(true);
                btnSdoSafe_->setEnabled(running_); lblMotionState_->setText("播放松闸失败");
                QMessageBox::warning(this, "动作播放",
                    QString("有 %1 个电机松开抱闸失败，已取消播放。")
                        .arg(failedPlaybackBrakes_));
            } else if (!worker_->beginMotionReturnToStart(pendingPlaybackMotion_, pendingPlaybackSpeed_)) {
                writeAllBrakes(false, "WPLAYCLOSE");
                tabs_->setEnabled(true); btnRecord_->setEnabled(true); btnPlay_->setEnabled(true);
                btnSdoSafe_->setEnabled(running_);
                QMessageBox::warning(this, "回起点", "松闸成功，但回起点任务启动失败");
            } else {
                playbackActive_ = true;
                appendLog(QString("全部电机抱闸已松开，以 %2 脉冲/s 回到动作“%1”的实际起点")
                    .arg(pendingPlaybackName_).arg(pendingPlaybackSpeed_));
            }
        }
    }

    /* 抱闸状态专用回显 */
    if (r.tag.contains("BRK") && motor >= 0 && motor < panels_.size()) {
        if (r.ok && r.tag.startsWith("RBRK")) {
            QString s;
            switch ((int)r.value) {
                case 0: s = "0 闭合(制动)"; break;
                case 1: s = "1 高压供电(松开)"; break;
                case 2: s = "2 低压保持(松开)"; break;
                default: s = QString("%1 未知").arg(r.value); break;
            }
            panels_[motor].lblBrakeState->setText(s);
        } else if (!r.ok) {
            panels_[motor].lblBrakeState->setText("读取失败");
        }
    }
    if (motor >= 0 && motor < panels_.size() && !r.tag.contains("BRK"))
        panels_[motor].lblSdoResult->setText(txt);
}

void MainWindow::onBrakeOpen(int motor) {
    if (!worker_) { QMessageBox::warning(this,"抱闸","请先启动主站"); return; }
    SdoJob j; j.slave = motor; j.index = 0x2014; j.subindex = 1;
    j.bits = 8; j.isSigned = false; j.write = true; j.value = 1;
    j.tag = QString("WBRK/M%1/open").arg(motor);
    worker_->postSdo(j);
    appendLog(QString("电机 %1 请求松开抱闸 (0x2014:01=1)").arg(motor));
    /* 稍后自动读一次状态 */
    QTimer::singleShot(200, this, [this,motor]{ onBrakeQuery(motor); });
}

void MainWindow::onBrakeClose(int motor) {
    if (!worker_) { QMessageBox::warning(this,"抱闸","请先启动主站"); return; }
    int reply = QMessageBox::question(this, "抱死抱闸",
        QString("电机 %1 正在运行时强制抱死会导致堵转！确认继续？").arg(motor),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;
    SdoJob j; j.slave = motor; j.index = 0x2014; j.subindex = 1;
    j.bits = 8; j.isSigned = false; j.write = true; j.value = 0;
    j.tag = QString("WBRK/M%1/close").arg(motor);
    worker_->postSdo(j);
    appendLog(QString("电机 %1 请求抱死抱闸 (0x2014:01=0)").arg(motor));
    QTimer::singleShot(200, this, [this,motor]{ onBrakeQuery(motor); });
}

void MainWindow::onBrakeQuery(int motor) {
    if (!worker_) return;
    SdoJob j; j.slave = motor; j.index = 0x2014; j.subindex = 2;
    j.bits = 8; j.isSigned = false; j.write = false;
    j.tag = QString("RBRK/M%1/state").arg(motor);
    worker_->postSdo(j);
}
