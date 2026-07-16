# EtherCAT 电机调试 GUI（IgH Master + Qt5）

基于意优伺服 EtherCAT（CiA402）应用手册编写，参考 `ethLab_test0108/main.cpp`。
支持 **1~N 台电机** 同时调试：状态监视、模式切换、使能/故障复位、
目标位置/速度/力矩下发、SDO 任意对象读写。

## 功能

- 顶部：配置从站数量、VendorID、ProductID、周期 (μs)，启动/停止主站
- 每台电机独立 Tab：
  - 状态：AL 状态、Status Word、实际位置 (0x6064) / 速度 (0x606C) / 力矩 (0x6077) / 错误码 (0x603F) / 模式显示 (0x6061)
  - 运动：模式选择 (CSP/CSV/CST/PP/PV)、目标值、点动增量、使能 / 关闭 / 故障复位 / 应用目标
  - SDO：任意索引/子索引，8/16/32 位，有/无符号的读写
- 实时线程 1ms 周期（`clock_nanosleep` 绝对时间），GUI 与 RT 线程通过互斥锁交换状态/命令
- CiA402 状态机自动推进（`0x06 → 0x07 → 0x0F`，Fault → 0x80 复位）

## 依赖

- Linux + 已安装 IgH EtherLab Master（默认路径 `/usr/local/etherlab`）
- Qt5 Widgets（`qtbase5-dev`）
- CMake ≥ 3.5

```bash
sudo apt install qtbase5-dev cmake build-essential
```

## 编译

```bash
cd ethLab_gui
mkdir -p build && cd build
cmake ..
make -j
```

## 运行

需要 root 权限（访问 EtherCAT 字符设备）：

```bash
sudo ./ethLab_gui
```

> 建议：赋予 `cap_sys_nice` / `cap_ipc_lock` 以获得更稳定的实时性，或使用 PREEMPT_RT 内核。

## 使用流程

1. 连接好从站并启动 IgH：`sudo /etc/init.d/ethercat start`
2. 确认 `ethercat slaves` 能看到从站
3. 启动本程序 → 填入从站数、VendorID、ProductID → **启动主站**
4. 等待日志出现 "所有从站进入 OP"
5. 在电机 Tab 选择模式 → 点 **使能** → 填目标值 → **应用**
6. 使用 SDO 区读写 0x6083（加速度）/ 0x6084（减速度）/ 0x6081（轮廓速度）/ 0x607D（软限位）等调试参数

## 默认 Vendor/Product

```
VENDOR_ID    = 0x00001097  （意优）
PRODUCT_CODE = 0x00002406
```

如从站不同请在界面上修改。

## 文件结构

```
ethLab_gui/
  CMakeLists.txt     构建
  main.cpp           入口
  mainwindow.{h,cpp} Qt GUI
  ecworker.{h,cpp}   RT 线程 + PDO/SDO 封装
```

## 注意事项

- SDO 请求在 RT 线程的周期末尾（每 100 周期 = 100ms）批处理，避免抖动
- 模式切换最好在 **关闭** 状态下进行，或选择支持 OP 中切换的驱动
- CSP 未使能时目标位置自动跟随实际位置，防止再次使能瞬间跳动
- PDO 映射与示例保持一致（RxPDO 0x1600, TxPDO 0x1A00）；如需修改请同步更新 `ecworker.cpp` 中 `g_pdo_entries`
