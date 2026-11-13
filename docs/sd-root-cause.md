# X96 Max+ 双机(#2) SD 卡文件系统无法挂载 —— 根因总结

日期：2026-11-13　固件：DEBUG 探针版 `3c2d13f3`（含 CMD3/CMD9 探针 + R2 字序修复）
证据链：串口日志 `boot20261113-1.log` … `boot20261113-7.log`（#2 已接串口，115200 8N1）

---

## 最终状态（log-7 验收）

**SD 文件系统已能挂载**：

```
FS0: Alias(s):HD1b:;BLK2:
    VenHw(B072F2F9-1405-4A13-A210-3E5A4DD3B520)/HD(1,MBR,0x8588A40A)
```

- SD 卡识别全链路：CMD0 → CMD8(echo 0x000001AA) → ACMD41(OCR 0xC0FF8000, bit31 ready)
  → CMD2(CID 真数据) → CMD3(RCA=0xAAAA, 卡自分配) → CMD9(真 CSD) → CMD16(512)
- BlockIo 几何：`BlockSize 512, LastBlock 3B723FF`（32GB 卡，0x3B723FF×512B ≈ 31.9GB ✓）
- MBR 分区 p1 被发现 → FAT 挂载成功 → FS0
- 零接触自动引导正常：`FSOpen: Open '\X96LinuxBoot.efi' Success`，loader 被执行
- loader 退场原因：`\vmlinuz-6.12.114-ophub: Not Found` —— **卡上还没有内核文件**（不是固件问题）

---

## 为什么之前"SD 文件系统无法加载"——四层根因（按发现顺序）

### 根因 1：VIM3 的卡检测门在 #2 上给出假阴性（MmcDxe 从未启动）

`MesonIsCardPresent()` 沿用 VIM3 的 GPIOC_6 CD 脚判断；#2 的板卡接线不同 → 永远返回
FALSE。EmbeddedPkg MmcDxe 的 `CheckCardsCallback` 门是 `IsCardPresent() == !Initialized`，
假阴性导致识别状态机**一次都不进**（`notifies=0`），Shell 里看到的 `dh` 几何全是
BlockIo Media 的未初始化默认值。

**修复**：SD host 的在卡判断交给识别本身（`IsCardPresent` 恒 TRUE），驱动里加
`CardPolls` 心跳计数（MmcDxe 绑定后每 200ms 轮询一次，计数即绑定活性证据）。

### 根因 2：ACMD41 ready 判定用错位（bit0 应为 bit31）

调相器锁相后判断"卡就绪"用了 `(Response[0] & 0xFF) != 0`，而 OCR 的 ready 位是
**bit31**。导致 ACMD41 循环永远认为没就绪，CMD2/CMD3 流程从不推进。
（同一时期还发现逐相位发布诊断时 `ZeroMem` 会把 MmcDxe 的状态机踪迹擦掉——诊断
自伤，用"跨 HwInit 保留计数器"修掉。）

**修复**：`(Response[0] & 0x80000000)`；修好后调相器第一次尝试即锁相
（co=0 tx=0 400kHz，真 echo 0x000001AA、真 OCR 0xC0FF8000、真 CID）。

### 根因 3：EmbeddedPkg 的 CMD3 参数请求 RCA=0，本卡自说自话

`MmcIdentificationMode()` 发 `CMD3 arg=0x00000001` —— RCA 请求字段（arg[31:16]）= 0，
非法请求。#2 这张 Samsung "E32G" 卡的行为：**无视 RCA 请求，自分配 RCA**，且 R6 响应
的 RCA 字段就是它真正监听的地址：

| 请求 RCA | 响应 RCA 字段（wire） | 后续 CMD9/13 寻址 |
|---|---|---|
| 0x0000 | 0xAAAA | ✓ 卡应答 |
| 0x0001（我们代管上报） | 0xAAAB | ✗ 超时——卡不在 RCA=1 |
| （wire 直通 0xAAAA） | 0xAAAA | ✓ 卡应答 |

实验三组数据交叉定案（log-3/4/5：`CMD9 arg=00010000 → resp 全零超时`）。

**修复**：wire RCA 直通——驱动不再改写 CMD3 响应的 RCA 字段，MmcDxe 用卡自报的
RCA 寻址。（曾试验"请求 RCA=1 并上报 RCA=1"，被 CMD9 超时证伪后回退。）

### 根因 4：R2 长响应字序反转毒害 CSD 解析 → BlockSize 256 → FAT 拒挂（最终根因）

`MesonReceiveResponse()` 对 R2 做 `Buffer[i] = Response[3-i]` 反转，但：

- 控制器锁存语义：`CMD_RSP3 = CSD/CID bits[127:96]（MSW）… CMD_RSP0 = bits[31:0]`
- EmbeddedPkg 的 CSD 宏按**自然序**取字段：`MMC_CSD_GET_CCC = Response[2]>>20`（CSD[95:84]）、
  `MMC_CSD_GET_READBLLEN = (Response[2]>>16)&0xF`（CSD[83:80]）、`TRAN_SPEED = Response[3]&0xFF`
- 用 log-6 捕获的真 CSD 字（`0A404000/EDC87F80/5B590000/400E0032`）验证自然序解析：
  CCC=0x5B5 ✓、read_bl_len=9→512 ✓、TRAN_SPEED=0x32(50MHz) ✓、structure=1(v2) ✓
- 反转时 read_bl_len 错读成 CSD[47:44]：#2 卡该 nibble=8 → `1<<8 = 256`；#1 卡恰好=9 → 512。
  **#1 的侥幸成功掩盖了字序 bug**。

256 字节几何被 FatDxe 拒绝（log 全链）：

```
FatAllocateVolume invalid BlockIo BlockSize 256 for FAT filesystem ...
    Must be 512B, 1KB, 2KB, or 4KB
```

**修复**：SD host 的 R2 响应按自然序直传（`3c2d13f3`）。FS0 随即出现。

---

## 附带修复/发现（过程性）

1. **MmcDxe 绑定时序**：`MesonInstallHost` 装完协议立即自 `ConnectController`，
   不再依赖 BDS connect-all 的先后顺序。
2. **CMD_CFG_RESP_NUM（bit22）+ 发命令前清 CMD_RSP**：对齐 mainline
   meson-gx-mmc 的语义（响应存在标志/陈旧锁存）。保留在树里。
3. **诊断体系**：共享头 `Silicon/Amlogic/MesonSm1/Include/MesonSdDiag.h`（pack(1)，
   双端零手算）；BDS 三行常显 + 倒计时 tick 行（notifies/polls/last-state/statemask/
   双 host cmdtrace）+ `d` 热键 FV 应用全字段打印 + CMD3/CMD9/CMD13/CMD7/eMMC 探针。
   串口上线后这些是未来排障的基础设施。
4. **这张卡的怪癖备忘**：RCA 自分配且跨暖复位保持；warm 状态可越过识别
   （log-4 上电直接以 standby 应答）；"High Speed not supported by Card"
   （400kHz 识别时钟下的正常提示，无害）。

---

## log-7 里"奇怪的地方"清单

| 现象 | 定性 |
|---|---|
| `eMMC CMD9 resp=8A404000/FFFFFFEF/8F5903FF/D00F0032` | eMMC 的 R2 字序**也是反转的**（自然序解码 TRAN_SPEED=0x32✓、read_bl_len=9✓）。但 eMMC 的几何来自 EXT_CSD（`Media->LastBlock = ECSDData->SECTOR_COUNT-1`，Mmc.h:245），不走 CSD，所以没暴露。**建议下版统一为自然序** |
| eMMC `MmcIoBlocks(): Failed to transfer block and Status:Time out`（反复出现） | eMMC **数据路径**（CMD17/18 读）不通——识别/几何正常但块读超时。独立专题：查 MesonSetIos 的 eMMC 传输时钟/相位、R1b busy 处理 |
| `EmmcSetEXTCSD(): Failed to send CMD6` | CMD6(R1b) 超时，与上一条同属 eMMC 数据/R1b 路径专题 |
| SD 卡 CMD13 响应 `0x900/0xB00/0x4000900` | 正常：stand-by(3)/tran(4) 状态位真实变化，卡活着 |
| `notifies=29` | 识别被多次重入（MmcDxe 检测定时器+多轮），无碍 |
| FS0 出现后 loader 立即退出 | 卡上没有 `\vmlinuz-6.12.114-ophub`——**下一步用 dufs 把内核+initrd 放进卡** |
| Shell `BLK0` = VenHw(9EC8EF97) 无分区 | eMMC 无 MBR/GPT（Amlogic 自定义分区表），符合预期 |

---

## 下一步

1. **dufs 阶段化内核**：把 `vmlinuz-6.12.114-ophub` + `initrd.img-6.12.114-ophub` PUT 到
   dufs 卡根（注意：dufs 覆盖旧文件要用 PUT 唯一名 → MOVE 覆盖配方），FS0 上 loader
   即可起 lophub 内核 → Armbian。
2. eMMC R2 字序统一为自然序（顺手，一行分支删除）。
3. eMMC 数据路径（CMD17/18/R1b）专题——修通后这台盒子连 Android 分区都能 UEFI 直接读。
4. 上述全部验证后，把 RELEASE 版固定为当前基线，回归 #1 确认无退化。

## 相关提交

- `MesonSdMmc: R2 response words in natural register order...`（`3c2d13f3` 家族，最终修复）
- `pass the wire CMD3 RCA through untouched`（RCA 自分配卡适配）
- `CMD_CFG_RESP_NUM for every response command and clear CMD_RSP before kick`
- `diag: dual-host cmd/state traces` + `CMD3/RCA probe` + `CMD7/9/10/13 debug probes`
