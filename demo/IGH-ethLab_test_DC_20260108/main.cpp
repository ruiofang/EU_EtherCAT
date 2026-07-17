#include <ecrt.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <stdbool.h>
#include <time.h>      // clock_gettime, CLOCK_MONOTONIC
#include <unistd.h>   // usleep（你已经在用）

/* ================= 用户配置 ================= */
#define SLAVE_NUM      4          // 🔴 电机数量
#define VENDOR_ID      0x00001097
#define PRODUCT_CODE   0x00002406
#define CYCLE_NS       2000000     // 2ms
/* ============================================ */

static int run = 1;

/* EtherCAT master */
static ec_master_t *master = NULL;
static ec_domain_t *domain = NULL;
static ec_slave_config_t *sc[SLAVE_NUM];
static uint8_t *domain_pd = NULL;

/* ================= PDO offset (数组) ================= */
static uint32_t control_word[SLAVE_NUM];
static uint32_t target_pos[SLAVE_NUM];
static uint32_t target_velocity[SLAVE_NUM];
static uint32_t target_tor[SLAVE_NUM];
static uint32_t op_mode[SLAVE_NUM];
static uint32_t resv1[SLAVE_NUM];

static uint32_t status_word[SLAVE_NUM];
static uint32_t actual_pos[SLAVE_NUM];
static uint32_t actual_velocity[SLAVE_NUM];
static uint32_t actual_tor[SLAVE_NUM];
static uint32_t op_mode_disp[SLAVE_NUM];
static uint32_t error_code[SLAVE_NUM];
static uint32_t resv2[SLAVE_NUM];
static ec_slave_config_state_t state[SLAVE_NUM];
/* ================= PDO 描述 ================= */
static ec_pdo_entry_info_t slave_pdo_entries[] = {
    {0x6040, 0x00, 16},
    {0x607A, 0x00, 32},
    {0x60FF, 0x00, 32},
    {0x6071, 0x00, 16},
    {0x6060, 0x00, 8},
    {0x60C2, 0x01, 8},

    {0x6041, 0x00, 16},
    {0x6064, 0x00, 32},
    {0x606C, 0x00, 32},
    {0x6077, 0x00, 16},
    {0x6061, 0x00, 8},
    {0x603F, 0x00, 16},
    {0x2026, 0x00, 8},
};

static ec_pdo_info_t slave_rx_pdo[] = {
    {0x1600, 6, slave_pdo_entries},
};

static ec_pdo_info_t slave_tx_pdo[] = {
    {0x1A00, 7, &slave_pdo_entries[6]},
};

static ec_sync_info_t slave_syncs[] = {
    {0, EC_DIR_OUTPUT, 0, NULL, EC_WD_DISABLE},
    {1, EC_DIR_INPUT,  0, NULL, EC_WD_DISABLE},
    {2, EC_DIR_OUTPUT, 1, slave_rx_pdo, EC_WD_ENABLE},
    {3, EC_DIR_INPUT,  1, slave_tx_pdo, EC_WD_DISABLE},
    {0xff}
};

/* ================= PDO 注册表 ================= */
static ec_pdo_entry_reg_t domain_regs[SLAVE_NUM * 13 + 1];

/* ================= 快捷访问宏 ================= */
#define CW(i) (*(uint16_t *)(domain_pd + control_word[i]))
#define TP(i) (*(int32_t  *)(domain_pd + target_pos[i]))
#define SW(i) (*(uint16_t *)(domain_pd + status_word[i]))
#define AP(i) (*(int32_t  *)(domain_pd + actual_pos[i]))
#define OP(i) (*(int8_t   *)(domain_pd + op_mode[i]))
#define ERR_CODE(i) (*(uint16_t *)(domain_pd + error_code[i]))
/* ================= 信号处理 ================= */
void signal_handler(int sig) {
    run = 0;
}

/* ================= 主函数 ================= */
int main(void) {
    signal(SIGINT, signal_handler);

    /* 1. 请求 master */
    master = ecrt_request_master(0);
    if (!master) return -1;

    ecrt_master_select_reference_clock(master, 0);  // 选择从站0作为参考时钟  DC

    domain = ecrt_master_create_domain(master);
    if (!domain) return -1;

    /* 2. 配置从站 */
    for (int i = 0; i < SLAVE_NUM; i++) {
        sc[i] = ecrt_master_slave_config(
            master, 0, i, VENDOR_ID, PRODUCT_CODE);
        if (!sc[i]) return -1;

        if (ecrt_slave_config_pdos(sc[i], EC_END, slave_syncs))
            return -1;
        ecrt_slave_config_dc(  //DC
            sc[i],
            0x0300,        // AssignActivate:
                        // bit8  = SYNC0 enable
                        // bit9  = SYNC1 disable
            2000000,      // sync0 cycle
            0,             // sync0 shift
            0,             // sync1 cycle (不用)
            0              // sync1 shift
        );
    }

    /* 3. PDO 注册 */
    int idx = 0;
    for (int i = 0; i < SLAVE_NUM; i++) {
        domain_regs[idx++] = (ec_pdo_entry_reg_t){0,i,VENDOR_ID,PRODUCT_CODE,0x6040,0,&control_word[i]};
        domain_regs[idx++] = (ec_pdo_entry_reg_t){0,i,VENDOR_ID,PRODUCT_CODE,0x607A,0,&target_pos[i]};
        domain_regs[idx++] = (ec_pdo_entry_reg_t){0,i,VENDOR_ID,PRODUCT_CODE,0x60FF,0,&target_velocity[i]};
        domain_regs[idx++] = (ec_pdo_entry_reg_t){0,i,VENDOR_ID,PRODUCT_CODE,0x6071,0,&target_tor[i]};
        domain_regs[idx++] = (ec_pdo_entry_reg_t){0,i,VENDOR_ID,PRODUCT_CODE,0x6060,0,&op_mode[i]};
        domain_regs[idx++] = (ec_pdo_entry_reg_t){0,i,VENDOR_ID,PRODUCT_CODE,0x60C2,1,&resv1[i]};

        domain_regs[idx++] = (ec_pdo_entry_reg_t){0,i,VENDOR_ID,PRODUCT_CODE,0x6041,0,&status_word[i]};
        domain_regs[idx++] = (ec_pdo_entry_reg_t){0,i,VENDOR_ID,PRODUCT_CODE,0x6064,0,&actual_pos[i]};
        domain_regs[idx++] = (ec_pdo_entry_reg_t){0,i,VENDOR_ID,PRODUCT_CODE,0x606C,0,&actual_velocity[i]};
        domain_regs[idx++] = (ec_pdo_entry_reg_t){0,i,VENDOR_ID,PRODUCT_CODE,0x6077,0,&actual_tor[i]};
        domain_regs[idx++] = (ec_pdo_entry_reg_t){0,i,VENDOR_ID,PRODUCT_CODE,0x6061,0,&op_mode_disp[i]};
        domain_regs[idx++] = (ec_pdo_entry_reg_t){0,i,VENDOR_ID,PRODUCT_CODE,0x603F,0,&error_code[i]};
        domain_regs[idx++] = (ec_pdo_entry_reg_t){0,i,VENDOR_ID,PRODUCT_CODE,0x2026,0,&resv2[i]};
    }
    domain_regs[idx] = (ec_pdo_entry_reg_t){};

    if (ecrt_domain_reg_pdo_entry_list(domain, domain_regs))
        return -1;
    // 在激活主站前，设置主站为DC主站
        /* 4. 启动 */
    if (ecrt_master_activate(master))
        return -1;

    domain_pd = ecrt_domain_data(domain);
    if (!domain_pd) {
        printf("domain data not ready\n");
        return -1;
    }

    int domain_size = ecrt_domain_size(domain);
    memset(domain_pd, 0, domain_size); 

    //DC同步稳定期
    // for (int i = 0; i < 2000; i++) {
    //     ecrt_master_receive(master);
    //     ecrt_domain_process(domain);
    //     ecrt_domain_queue(domain);
    //     ecrt_master_send(master);
    //     usleep(1000);
    // }

 /* 5. 等待所有从站 OP */
    for (int i = 0; i < 5000; i++) {   // 500 × 2ms = 1s
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t app_time =
            (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

        ecrt_master_application_time(master, app_time);  //DC

        ecrt_master_receive(master);
        
        /* 只 process / queue，不写任何 PDO */
        ecrt_domain_process(domain);
        ecrt_domain_queue(domain);
        
        ecrt_master_send(master);
        int safeop_cnt = 0;
        if (i % 100 == 0)
        {
            for (int j = 0; j < SLAVE_NUM; j++) {
                ecrt_slave_config_state(sc[j], &state[j]);
                if (state[j].al_state == EC_AL_STATE_OP)
                    safeop_cnt++;
            }

            if (safeop_cnt == SLAVE_NUM) {
                printf("all slaves OP\n");
                break;
            }
        }
        usleep(500);  //周期越快越稳定，防止触发边界进op异常，后面通信周期可按需切换其他
    }

    printf("Multi-motor EtherCAT started (%d motors)\n", SLAVE_NUM);

    /* ================= 控制变量 ================= */
    int32_t start_pos[SLAVE_NUM] = {0};
    bool enabled[SLAVE_NUM] = {false};
    int step = 1000;

    /* ================= 主循环 ================= */
    bool init = false;
    while (run) {
        ecrt_master_receive(master);
        ecrt_domain_process(domain);
            
        for (int i = 0; i < SLAVE_NUM; i++) {
            OP(i) = 8; // CSP

            uint16_t status = SW(i);
            uint16_t control = 6;

            /*  Fault 优先处理 */
            if (status & (1 << 3)) {   // bit3 = Fault
                control = 0x80;        // Fault Reset
            }
            else {
                /*  正常 CiA402 状态机 */
                switch (status & 0x6F) {
                    case 0x00:
                    case 0x40:
                        control = 0x06;
                        break;

                    case 0x21:
                        control = 0x07;
                        start_pos[i] = AP(i);
                        printf("srart Pos:%d (%d motors)\n",start_pos[i], i);
                        TP(i) = start_pos[i];
                        break;

                    case 0x23:
                        control = 0x0F;
                        break;

                    case 0x27:
                        control = 0x0F;
                        enabled[i] = true;
                        break;

                    default:
                        control = 0x06;
                        break;
                }
            }
            CW(i) = control;

            if (enabled[i]) {
                start_pos[i] += step;
                TP(i) = start_pos[i];
            }
        }

        ecrt_domain_queue(domain);
        ecrt_master_send(master);
        usleep(2000);
    }

    ecrt_master_deactivate(master);
    ecrt_release_master(master);
    return 0;
}

