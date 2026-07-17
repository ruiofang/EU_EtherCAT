# EtherCAT 电机调试 GUI（IgH Master + Qt5）

基于意优伺服 EtherCAT（CiA402）应用手册编写，参考 `ethLab_test0108/main.cpp`。
支持 **1~N 台电机** 同时调试：状态监视、模式切换、使能/故障复位、
目标位置/速度/力矩下发、SDO 任意对象读写。

## 功能

- 顶部：配置从站数量、VendorID、ProductID、周期 (μs)，启动/停止主站
- 机械臂动作示教：多轴位置录制、命名持久化、限速回起点及同步播放
- 每台电机独立 Tab：
  - 状态：AL 状态、Status Word、实际位置 (0x6064) / 速度 (0x606C) / 力矩 (0x6077) / 错误码 (0x603F) / 模式显示 (0x6061)
  - 运动：模式选择 (CSP/CSV/CST/PP/PV)、目标值、点动增量、使能 / 关闭 / 故障复位 / 应用目标
  - SDO：任意索引/子索引，8/16/32 位，有/无符号的读写
- 实时线程默认 1ms 周期（可在 GUI 配置，`clock_nanosleep` 绝对时间），GUI 与 RT 线程通过互斥锁交换状态/命令
- 电机使用 EtherCAT Distributed Clocks：`AssignActivate=0x0300`，SYNC0 周期跟随控制周期；自动选择总线上第一个实际电机作为参考时钟，进入 OP 前以 500μs 收发节拍持续校正参考钟和各从站时钟
- CiA402 状态机自动推进（`0x06 → 0x07 → 0x0F`，Fault → 0x80 复位）
- 主站启动成功后自动使能全部已识别 EU 电机，默认使用 CSP 捕获并保持当前位置，避免使能瞬间跳变；GUI Status Word 和 CLI `ENABLED=YES/NO` 显示实际使能状态
- 退出受控停机：关闭 GUI、停止主站或 CLI 退出时逐轴保留原使能状态；已失能轴保持 Shutdown，已使能轴保持 Operation Enabled。若正在运动，则先取消轨迹、切换 CSP 锁定当前反馈位置并将速度/力矩目标清零，实际速度归零后再释放主站（异常时最多等待 1 秒）
- SDO 使用独立线程处理；退出时取消排队请求，并在当前 SDO 完成或超时前继续维持 PDO 通信，避免停止过程死锁
- 进入 OP 时单次最多等待 15 秒；若 OP 数量和 Domain Working Counter 连续 6 秒都没有增长，会提前完整释放 Master。程序最多尝试 3 次，每次重试前等待 800ms，最后一次仍失败才报告启动错误
- GUI 可选择是否保存日志；GUI、CLI、配置和动作库均统一使用可执行文件所在目录

## 依赖

- Linux + 已安装 IgH EtherLab Master（默认路径 `/usr/local/etherlab`）
- Qt5 Widgets（`qtbase5-dev`）
- CMake ≥ 3.5

```bash
sudo apt install qtbase5-dev cmake build-essential
```

## 编译

```bash
cmake -S EU_robot_arm_gui -B EU_robot_arm_gui/build
cmake --build EU_robot_arm_gui/build -j
```

## 运行

需要 root 权限（访问 EtherCAT 字符设备）：

```bash
sudo -E ./EU_robot_arm_gui/build/EU_robot_arm_gui
```

> 建议：赋予 `cap_sys_nice` / `cap_ipc_lock` 以获得更稳定的实时性，或使用 PREEMPT_RT 内核。

## 程序目录文件

GUI 和 CLI 只使用可执行文件所在目录中的配置、日志和动作文件，不会在
`~/.config`、`~/.local/share` 或 `/root/.local/share` 中创建副本：

```text
build/
  EU_robot_arm_gui.conf   # 共用配置，saveLog=true/false
  robot_motions.json      # GUI/CLI 共用动作库，唯一存储位置
  log/
    yyyyMMdd_HHmmss_zzz.log
    yyyyMMdd_HHmmss_zzz_cli.log
```

通过 `sudo` 启动时，程序会根据 `SUDO_UID/SUDO_GID` 将上述目录和文件归还
给发起 `sudo` 的普通用户。配置和数据文件权限为 `0664`，日志目录权限为
`0775`，后续可直接用普通用户编辑。

GUI 顶部的“保存日志”复选框控制 `saveLog`。CLI 读取同一配置：关闭时仍
保留终端输出，但不创建 CLI 日志；开启时普通 EtherCAT 诊断只写文件，不刷屏。

## 使用流程

1. 连接好从站并启动 IgH：`sudo /etc/init.d/ethercat start`
2. 确认 `ethercat slaves` 能看到从站
3. 启动 GUI，点击“扫描从站”，确认电机 Vendor/Product、Revision 和实际接线一致
4. 点击“启动主站”，等待日志显示全部电机进入 OP，再进行使能或运动操作

## 混合设备与 EC 分支拓扑

程序允许 EtherCAT 总线中同时存在电机、EC 分支器、I/O 和末端工具等不同设备。
非电机设备可以位于首端、中间或末端，不要求电机从站连续排列：

```text
Master
  └─ EC 分支器 / HUB
       ├─ EU 电机 ...
       ├─ EU 电机 ... ─ 末端工具
       └─ 其它 EtherCAT 设备
```

- 每次启动都会重新读取所有位置的实时 Vendor/Product；扫描阶段保存的电机掩码只作为提示，不会将位置变化后的附件误配置成电机。
- 仅匹配当前 EU 电机 Vendor/Product 的从站创建 `slave_config`、注册电机 PDO、执行 CiA402、SDO 抱闸和动作控制。
- 分支器和其它非电机设备不注册电机 PDO，也不要求提供本项目支持的 XML；扫描日志中的 `XML=<不支持>` 只表示 GUI 没有该设备的 ESI 显示/配置文件，不代表它不能透明转发 EtherCAT 帧。
- DC 参考时钟选择总线上遇到的第一个实际电机，不固定为物理位置 `#0`，因此首端增加分支器或其它设备后仍可同步。
- 分支拓扑会改变各支路传播延迟。程序在 PREOP→OP 阶段就持续调用参考时钟及从站时钟同步，不能等到全部 OP 后再开始；否则 IgH 可能对每个 DC 从站依次等待约 5 秒，表现为“每隔约 5 秒才有一个轴进入 OP”。

使用分支器仍需保证它是标准 EtherCAT Junction/Branch 设备，SII 中的端口和拓扑信息正确，且各支路网线、屏蔽、接地和端口 Link 状态正常。普通以太网交换机不能替代 EtherCAT 分支器。

成功启动时日志应包含类似内容：

```text
DC同步已配置：参考电机从站 #1，SYNC0=1000000ns
启动配置：收发节拍=500us，停滞阈值=6000ms，单次超时=15000ms；DC 时钟在进 OP 阶段持续同步
全部 9 个电机从站进入 OP
```

## 无法进入 OP 的诊断

日志中的 AL 状态含义：`0x02=PREOP`、`0x04=SAFEOP`、`0x08=OP`。判断故障时同时观察：

- `master响应`：应等于当前总线可见的全部从站数量；数量变化通常指向链路、供电、接头或分支端口问题。
- `online`：所有预期设备应保持 `online=1`。
- `domainWC`：应随进入 SAFEOP/OP 的电机逐步增长，全部 PDO 生效后 WC 状态应完成。
- 卡住的位置是否每次固定：固定在同一轴更像该轴 PDO/DC/固件或其前一段链路问题；每次位置不同但所有设备始终在线，更像实时性、DC 收敛或物理层偶发错误。

程序失败并释放 Master 后执行以下命令，并保存完整输出：

```bash
sudo ethercat master
sudo ethercat slaves -v
sudo dmesg -T | grep -i ethercat | tail -200
```

还应检查：

1. GUI 是否以 `sudo` 运行，并出现 `RT 线程已设置 SCHED_FIFO priority=80`；`SCHED_FIFO` 或 `mlockall` 失败会增加抖动。
2. IgH 配置绑定的是否为实际 EtherCAT 网卡，而不是不存在的 `eth0`；可用 `ip link` 和 `ethtool -i <网卡名>` 确认。
3. EtherCAT 专用网卡不要同时交给 NetworkManager、DHCP 或普通 IP 网络使用，并按网卡/IgH 驱动要求关闭可能合并或延迟报文的 offload 功能。
4. 对照测试时可临时绕过分支器直连一台电机。如果直连稳定而接入某一分支后失败，重点检查该分支器端口、SII 拓扑、支路线缆和供电；如果相同分支拓扑曾经成功全部进入 OP，则不能仅凭 `XML=<不支持>` 判定分支器不兼容。

## 机械臂动作录制与播放

1. 启动主站，确认所有 EU 电机进入 OP，机械臂处于可安全手动拖动的环境。
2. 根据机构需要先处理抱闸，然后点击“开始录制（失能）”。录制期间程序持续发送失能控制字，只采集各轴 `0x6064` 实际位置。默认每 20 ms 采一帧（50 Hz）。
3. 手动拖动机械臂完成示教，再点击“停止录制并保存”，输入自定义动作名称。动作以 JSON 保存在可执行文件同目录的 `robot_motions.json`，GUI 与 CLI 共用，支持多条动作、覆盖和删除。通过 `sudo` 运行时文件会自动归还给发起 `sudo` 的普通用户。
4. 选择动作并设置“回起点速度”（编码器脉冲/秒），点击“回起点”。程序会松开抱闸、以 CSP 同步使能各轴并限速移动到首帧；状态显示“已到起点”后，点击独立的“播放”按钮按录制时间执行轨迹。
5. 回起点或播放期间可随时点击“停止动作”；所有轴退出使能请求并停止轨迹执行。

机械臂动作区提供独立控制按钮，并保留组合快捷操作：

- `使能`：向全部已识别 EU 电机发送 CiA402 使能请求。
- `一键故障复位`：失能全部电机，并向所有已识别 EU 电机发送一次 CiA402 `Controlword=0x0080` 故障复位脉冲；复位后不会自动使能。
- `失能并松开抱闸`：停止当前轨迹、失能全部电机，再写 `0x2014:01=1`，用于手动摆动机械臂。
- `回起点`：针对所选动作松开抱闸并限速移动到轨迹首帧，到位后保持位置，不自动播放。
- `播放`：仅在状态显示“已到起点”后启动所选轨迹。
- `回起点并播放`：执行与“回起点”完全相同的松闸、限速和实际位置到位检查，到位后自动开始播放。

抱闸控制采用兼容/未知策略：录制、回起点或播放前会尝试向已识别电机写入
`0x2014:01=1`。“失能并松开抱闸”按钮以及录制、回起点、播放流程中，部分或全部
电机写入失败时，界面会汇总失败数量并弹出安全警告，随后仍继续当前流程，以兼容
没有物理抱闸或未实现该对象的电机；电机失能本身不受抱闸 SDO 结果影响。操作者必须确认
相关电机确实没有物理抱闸，或抱闸已经安全松开。

注意：首次在真实机械臂测试时请使用较低回起点速度，确认各轴方向、编码器单位、机械限位、抱闸和急停有效。动作文件记录的是编码器绝对位置，轴数不匹配时程序会拒绝播放。

详细实现约束见 `机械臂动作录制播放提示词.md`。

## CLI 无界面模式

项目同时生成 `EU_robot_arm_cli`，适用于没有桌面环境的工控机或 SSH 终端。CLI 与 GUI 共用 `EcWorker`、EtherCAT 配置和 `robot_motions.json` 动作库。

CLI 同样采用兼容/未知抱闸策略：菜单 5、录制和回起点会等待全部 `0x2014:01`
写入尝试返回；失败时在终端汇总输出安全警告，但仍继续对应流程。

启动：

```bash
cd /home/ruio/data/123/EU_EtherCAT
sudo -E ./EU_robot_arm_gui/build/EU_robot_arm_cli
```

程序启动后会自动扫描并占用 EtherCAT Master 0。GUI 和 CLI 不能同时运行。

CLI 强制使用 UTF-8 输出，不依赖 `sudo` 环境中的 locale；启动时会在最多 10 秒内自动重试扫描从站，避免 EtherCAT 服务或链路状态机尚未就绪时立即退出。

CLI 默认不输出周期诊断等普通 EtherCAT 日志，只显示错误、SDO失败、动作状态转换和用户主动查询结果，减少终端 I/O 对实时周期的影响。

启动后使用数字菜单：

```text
 1. 查看电机状态
 2. 查看动作列表
 3. 全部使能
 4. 一键故障复位
 5. 全部失能并松开抱闸
 6. 开始录制
 7. 停止录制并命名保存
 8. 回到动作起点
 9. 播放（需先到起点）
10. 回起点并播放
11. 停止当前动作
12. 设置采样周期/回起点速度
 0. 退出
```

选择 8 或 10 后，程序会显示带编号的动作列表，再输入动作编号；选择 7 后会提示输入动作名称；选择 12 后按提示输入采样周期和回起点速度。所有追加输入都由非阻塞状态机处理，不会因为等待终端输入而暂停 EtherCAT 周期线程。

交互示例：

```text
请选择> 12
输入：采样周期ms 回起点速度（例如 20 50000）> 20 50000
请选择> 10
动作编号（0取消）> 2
```
## 停止与退出

- 点击“停止主站”、关闭 GUI 窗口、收到 `SIGINT`/`SIGTERM`，以及 CLI 选择 `0. 退出`，都会进入相同的受控停机流程。
- 实时线程会立即中止正在执行的回起点或轨迹播放，用 CSP 锁定当前反馈位置，并至少持续发送 3 个 EtherCAT 周期。
- 当全部已识别电机的实际速度接近零后，程序才释放 EtherCAT Master；若驱动未在 1 秒内反馈停止，程序仍会继续退出，避免界面或 CLI 无限等待。
- 若停止时已有阻塞式 SDO 正在执行，排队中的 SDO 会立即取消，但 PDO 周期会继续运行直到当前 SDO 返回。当前设备的 SDO 超时通常约 9 秒，因此停止可能延迟，但不会形成“先停 PDO、再无限等待 SDO”的死锁。
- 主站释放后能否继续保持使能，由驱动器 EtherCAT watchdog/通讯中断策略决定。真实机械臂必须保留独立硬件急停和抱闸等安全回路，不能将软件退出流程作为安全功能的替代。

## 默认 Vendor/Product

```
VENDOR_ID    = 0x00001097  （意优）
PRODUCT_CODE = 0x00002406
```

如从站不同请在界面上修改。

## 文件结构

```
EU_robot_arm_gui/
  CMakeLists.txt     构建
  main.cpp           入口
  mainwindow.{h,cpp} Qt GUI
  ecworker.{h,cpp}   RT 线程 + PDO/SDO 封装
```

## 注意事项

- SDO 请求在独立线程中串行处理，避免阻塞 EtherCAT 实时周期；停止过程中不再接受新请求
- 抱闸 SDO `0x2014` 只发送给识别出的 EU 电机，不会查询 EtherCAT HUB 或不匹配的产品型号
- 模式切换最好在 **关闭** 状态下进行，或选择支持 OP 中切换的驱动
- CSP 未使能时目标位置自动跟随实际位置，防止再次使能瞬间跳动
- 扫描会读取电机 EEPROM `RevisionNo`：V143 绑定 `xml/EYOU_ServoModule_ECAT_V143.xml`，V145 绑定 `xml/EYOU_ServoModule_ECAT_V145.xml`；其他版本会拒绝启动，避免使用错误 PDO 定义
- PDO 映射与示例保持一致（RxPDO 0x1600, TxPDO 0x1A00）；如需修改请同步更新 `ecworker.cpp` 中 `g_pdo_entries`
