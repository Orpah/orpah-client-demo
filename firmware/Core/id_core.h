/* id_core.h — 已签 ID 上报**任务**（c4-γ-1）：周期组装 → 走模组数据口发出。
 *
 * 分工（别把这三层搅在一起）：
 *   · `proto/id_build.c`  = 流水线本体（选级 → 取钥 → nonce → idr_build → 信封 → 以太帧）
 *                           **与 PC 侧交叉测试同一份源码**
 *   · 本文件              = **固件侧的策略**：什么时候发、自限频、控制台、计数
 *   · `Periph/hgic_uart.c`= 真正把字节写进模组数据口
 *
 * ★ 单一源（别在这里改数值）：
 *   · 周期 **60 s** = 设计常态，单一源 = `orpah-over-halow/energy.NORMAL_INTERVAL_S`
 *     （协议 SPEC §5.2 末段：正常情况每 60 s 连一次路由器）
 *   · 自限频最小间隔 = 上游 `ORPAH_SELF_*` 的默认 0.6 s；**语义是延后、不是丢弃**（§5.8 设备侧）
 *   · 选级/故障注入模式名 = §8.2 + 上游 `LEVEL_MODES`（见 `proto/id_level.h`）
 *
 * ⚠ **如实**：本 demo 的 `se_ok` 是**软件 P-256 顶替**（ROADMAP §五 已定：SE 驱动在 d 步）。
 *   也就是说这里的 level=0 是**演示级**（密钥在 MCU 内、非 SE 保护），必须对用户说明；
 *   d 步接上 ATECC608B 后同一条路径直接换成真 SE（改 `_se_ok()` 一处）。
 * ⚠ RAM：单片 20 KB ⇒ jcs arena + 三个缓冲都是 static（**别放栈上**）。
 */
#ifndef ORPAH_ID_CORE_H
#define ORPAH_ID_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "id_build.h"      /* 缓冲推荐值 IDB_REC_* 的单一源 */

#define IDC_SN_DEFAULT   "CN-WH01-9AF3C1D2"    /* 与上游 demo/向量同一个 SN（CC 只用 CN）*/
#define IDC_GEN_DEFAULT  1                     /* 演示"代次"；真机一代终身（§6.3.1）*/
#define IDC_FW_VERSION   "0.1.0-c4g1"          /* 写进已签 payload.firmware */

#define IDC_INTERVAL_MS  60000u                /* 60 s（见文件头"单一源"）*/
#define IDC_MIN_GAP_MS   600u                  /* 自限频：两次之间的最小间隔（延后不丢）*/

/* 缓冲（按实测长度留了余量：报文 ≤616 B、信封 ≤~700 B、帧 790 B；见 proto/README.md）
 * ★ 数值的单一源 = `proto/id_build.h` 的 `IDB_REC_*`（C 侧自检就是按它跑的）*/
#define IDC_REPORT_CAP   IDB_REC_REPORT_CAP
#define IDC_ENV_CAP      IDB_REC_ENV_CAP
#define IDC_FRAME_CAP    IDB_REC_FRAME_CAP

/* 初始化（`boot_entropy` = 上电熵，见 id_nonce.h 的"非生产强度"说明）。返回 0 = OK。*/
int  idc_init(uint32_t boot_entropy);

/* 主循环每圈调用：到点（`IDC_INTERVAL_MS`）就组装并发一条。返回本次是否发了。*/
int  idc_tick(uint32_t now_ms);

/* 控制台：认识 `id` / `idsend` / `idhex` / `idmodes` / `idlevel <模式>` 时返回 1（已处理）。
 * `now_ms` = 调用方的 ms 时基（不引 main 的全局变量，保持模块边界干净）。*/
int  idc_console(const char *cmd, uint32_t now_ms);

#endif /* ORPAH_ID_CORE_H */
