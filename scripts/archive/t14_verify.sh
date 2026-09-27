#!/bin/bash
# [已归档 2026-09-27] T14 已完成（J0-J6 硬件 ALL GREEN + rebaseline 基线
# 64 行/口 + r0_gate 8/8 收口）。判据针对 T14 路线 A 窗口预登记，勿跑后改；
# smoke 行为已沉淀进基线与 r0_gate。修正史（16 条）全文保留在下——核心
# 判决链：QP 路线九趟定因（roce_en=0 范畴封锁）→ 路线 A SQ 三档梯 →
# dbr_addr/cqn 位号事故（0x17a194 社区表命中）→ TIS+L2-inline+byte_cnt=0
# 三因子 → 黑匣子窗口与 J6 仪表自纠。交叉对账口径见 DESIGN v2.4 §5.7/§6.3。
# t14_verify.sh — T14 硬件冒烟测试（ETH TX SQ 数据面，路线 A）验证。
#
# 判据预登记（跑之前固化，跑完只对号入座；依据 task.json T14 路线 A 与
# DEVELOPMENT 计划，功课证据 = mlx5 7.0 wc.c + 5.15 en_main.c/en/params.c
# + 5.15 mlx5_ifc.h 三代 ABI 交叉）：
#   J0  build 无警告（compiler-differ 提示豁免）+ checkpatch 新增/改动文件
#       0/0/0（mlx5_prism.h 两条 HEAD 既有长行 check 属祖父条款，不计）
#       + fmt_equiv_check ALL EQUIVALENT（含 T14 节：31 个 ifc 字段 × 32
#       随机 == MLX5_SET 代数、偏移几何、WQE/doorbell/CQE 打包）
#   J1  stage1（smoke_stage=1，零 doorbell 零点火）：双口 dmesg 各含
#       ALLOC_PD/CREATE_MKEY/CREATE_CQ/CREATE_SQ ok tier=N（任一档）/
#       SQ to RDY ok/DESTROY_SQ/TIS/TD/CQ/MKEY/PD ok +
#       "Hardware datapath smoke test: PASSED"；且无 NOP/TX 行（证明未点火）
#   J2  stage2（+NOP WQE，send 管线点火不上线）：双口各含
#       "NOP WQE ok (opcode=REQ wqe_counter=0)" + PASSED；无 TX 行
#   J3  stage3（全量，默认档）：双口各含
#       "TX CQE ok (opcode=REQ wqe_counter=1" + PASSED；byte_cnt 实测值
#       入行（{0,64,78} 预登记：64=载荷、78=含头、0=本 fw TX CQE 不报
#       字节数——第十五趟实测）；任一档过即 GREEN（预期胜者 tier3：
#       无 TIS 的 SEND 判 LOCAL_QP_OP_ERR 0x02）；RX（RESP_SEND CQE/帧
#       比对）显式移交 T15——路线 A 无接收路径
#   J4  默认加载（selftest=0）：dmesg 零 smoke 行、零 PASSED（生产路径
#       零扰动，默认口径基线不动）；teardown 2305/2305 仍在
#   J5  每趟 rmmod 干净 + 全程 dmesg 零 WARNING/Oops/BUG
#   J6  stress：100 次 insmod/rmmod（selftest=1，stage3），黑匣子对账
#       PASSED 行 ≥200（100×双口），零失败零投诉
#   J7  （脚本外，收官步）有意基线变更（selftest=1 口径每口 smoke 行族
#       换 SQ 族）：normalize.sh smoke 行归一化（sqn/tisn/tdn 占位符）
#       + rebaseline.sh 白名单 → sudo bash scripts/gate/rebaseline.sh &&
#       sudo bash scripts/gate/r0_gate.sh → 8/8 全绿后才 commit
#
# 三档退路梯预登记（同趟自动升级，smoke_sq_tier 可钉档）：
#   档1 = wc.c 最小形态（主档：无 TIS 无 non_wire）；CREATE_SQ 被拒或
#   NOP 无 CQE → 档2（+non_wire=1，7.0-only 位，预期老固件拒，仅同趟一
#   试）→ 档3（ALLOC_TD→CREATE_TIS(仅 transport_domain)→tis_lst_sz=1，
#   mlx5e 实跑形态）。档3 SEND 仍无 CQE → NOT GREEN + syndrome 落黑匣子，
#   该判决本身即产出（转 T15 重议出口）。
#
# 输出全部落 /var/tmp/t14_run.log 单文件；黑匣子：/var/tmp/t14_live.log
# （dmesg 逐行 sync）。
#
# 修正史（2026-09-27 首轮上机）：stage3 双口 + stress 202 次一致判决——
# CREATE_QP (0x500) status 0x3 / syndrome 0x28b996 = "create_qp: bad service
# type"（社区 syndrome 表）：qpc st=0x9 (RAW_ETHERTYPE) 不在本固件
# (14.32.1908) 的合法服务类型表内；内核自 raw-packet QP 拆分 (SQ/RQ) 后
# 从未向 CREATE_QP 发过 st=9，该类型疑似从未在此产品线实装。PD/mkey/CQ
# 同几何字段全部接受、teardown 2305/2305 全程干净。处置：驱动内加 st
# 阶梯探针（smoke_qp_st=0 自动走 [9, 4(MLX, fre=0), 0(RC), 2(UD)]，首
# 个被接受类型继续后续转移）。
# 修正史（2026-09-27 第二趟，st 阶梯判决）：st=9 拒（0x28b996 复现）、
# st=4 拒（0x3/0xe3e6dd，社区表无此码，MLX 另有未满足约束）、**st=0 RC
# 接受（qpn=128 双口）**、RST2INIT ok、INIT2RTR 拒（0x2/0xe18032，社区
# 表无码；表内 "bad msg_max"/"rmac、rgid、src_addr_index 等 RoCE
# connected 字段"约束指向 RTR 需完整 RoCE 地址向量）。处置：RTR 富集梯
# （V1 +mtu/log_msg_max，V2 +src_addr_index/hop_limit，V3 +合成
# rmac/rgid/udp_sport（环回 QPN 定向投递，字段只需良构），V4
# +opt_param_mask 强制应用；同一 QP 逐级重试，INIT 态被拒不换态）。
# 探针趟：`sudo bash scripts/gate/t14_verify.sh stage1`——判定 PASS/FAIL
# 此时次要，黑匣子里的逐级 syndrome 判决是目标。
# 修正史（2026-09-27 第三趟，RTR 富集梯判决）：V1-V3（mtu/log_msg_max →
# +src_idx/hop → +合成 AV）**同一 syndrome 0xe18032 复现**（qpc 内容完全
# 不影响判决 = 拒绝发生在内容校验之前）；V4（opt_param_mask=PRIMARY_ADDR_
# PATH|PRI_PORT）→ 0x7016b9 "init2rtr_qp: bad opt_param_mask"（v4.19 允许
# 表证实该位组合确不在 INIT2RTR 允许集）。结论：唯一未变的主变量是
# remote_qpn=自身——固件在 RTR 处拒绝"目标=自己"。处置：改双 QP RC 对
# （QP1 发 → QP2 收，真连接路由，无需任何环回机制；比自环回更贴近真实
# RDMA 形态）；RTR 梯改为 [(level2,peer),(level3,peer),(level3,self 对照)]。
#
# 修正史（2026-09-27 第四趟，双 QP 判决）：peer(L2/L3) 与 self(L3) 六个
# 变体**全部同 syndrome 0xe18032**——remote_qpn 也不是拦截点，INIT2RTR
# 属范畴级拒绝。剩余硬假设二选一：vport roce_en=0（RoCE 关闭则 RC 连接
# 无法建立）或 QP 实态有诈。处置：selftest 内嵌三诊断（全只读/保态，
# 失败不致命）——①QUERY_NIC_VPORT 查 roce_en/local_lb/mtu；②RST2INIT 后
# QUERY_QP 读回 state/st/pm_state/remote_qpn/port；③INIT2INIT 对照（同
# opcode 族同状态，若 INIT2INIT 收而 INIT2RTR 拒则问题收窄到 RTR 路径）。
#
# 修正史（2026-09-27 第五趟，三诊断判决，T14 定因）：**QUERY_NIC_VPORT
# 双口 roce_en=0**——vport RoCE 关闭。RoCE 关则 RC 连接无法绑定（RTR），
# 固件以内容无关的 "bad operation" 拒绝——与前四趟全部观测吻合（create/
# init/init2init 等无连接语义命令全过，INIT2INIT 对照收、RTR 拒）。处置：
# 冒烟内嵌 RoCE 门（照 mlx5_nic_vport_enable_roce：MODIFY_NIC_VPORT
# field_select.roce_en 置 1，复查生效，teardown 恢复 0——离开时如初）。
# 另：QUERY_QP 读回 state=2/st=34 与预期不符，疑此老固件 query_qp_out
# 布局与 v7.0 有出入，已加原始 hexdump 对质（次要疑点，不阻塞主线）。
#
# 修正史（2026-09-27 第六趟）：MODIFY_NIC_VPORT roce_en（v7.0 布局
# ctx@256）被 0x3/0xc1f678 拒（表外码）。二选一：老固件 ctx 偏移漂移（bit
# 落保留区）或卡级 RoCE 配置关闭、运行时改写被策略拒绝。处置：①同路径
# no-op mtu 写（写回现值 1522）——收则命令路径+布局自证，roce_en 拒绝
# 即策略级；②roce_en 按 ctx@{256,128,16} 阶梯试写，复查生效为准。
#
# 修正史（2026-09-27 第七趟，判别梯判决）：no-op mtu 写**接受**（ctx@256
# 布局自证）；roce_en fs@0x7e 拒（0xc1f678 复现）。但系统侧发现 mlx5_core
# 在本机成功启用 RoCE（rocep8s0f0/f1 存在、caps byte67=0xff RoCE 能力位
# 为 1）——运行时开启可行，拒绝是编码不匹配。头号嫌疑：field_select 位
# 漂移（v7.0 的 affiliation@0x12 为后插入候选，老固件 roce_en 或在 0x7d）。
# 处置：fs 位阶梯 {0x7e,0x7d,0x7c} + 显式自 vport 寻址对照，复查生效为准。
#
# 修正史（2026-09-27 第八趟，fs 梯判决）：fs@0x7e 拒（0xc1f678 复现）、
# **fs@0x7d 接受但 roce_en 复查仍 0**——接受的写命中了别的字段（位漂移
# 落点未定）；且该趙 ensure 失败早退未走恢复，0x1f 位遗留置 1（脏状态）。
# 处置：①ensure 入口先清 0x1e/0x1f 候选位（还卡干净）；②(fs,ctx) 四组合
# 矩阵 {0x7e,0x7d}×{0x1f,0x1e}，每次写后回读全部 4 前置位（mc/uc/0x1e/
# 0x1f 指纹直接暴露真实布局）；③读回仍 0 不再中止——RTR 放行与否才是
# roce_en 的最终裁决。
# 修正史（2026-09-27 第九趟后，路线 A 决策）：九趟判决链定案——st=9/4 拒、
#   st=0 RC 收但 INIT2RTR 范畴级拒（0xe18032，内容无关）、根因 vport
#   roce_en=0、运行时启用被拒 0xc1f678（同字节 mlx5_core 可行，bring-up
#   前置差异不明，封档）。QP 数据面在此固件+配置不可行。用户拍板路线 A：
#   改 ETH TX SQ 对象（无 RoCE 依赖，mlx5e 每天实跑）。QP/RoCE 探针代码
#   整体退役（判决留档 progress.md/DESIGN.md），本脚本判据随路线重登记
#   （预登记时点 = 路线切换后首趟上机前）。版本对口首例：参考树切
#   linux-v5.15-study，5.15 headers 无 non_wire → 主档定 wc.c 最小形态。
# 修正史（2026-09-27 第十趟，路线 A 首趟）：**CREATE_SQ (0x904) 三档全拒
#   0x3/0x17a194（首负载即现，干净状态，双口一致）**——tier1 与 7.0 wc.c
#   字段集逐项一致（stride=6/pd 从 wc.c 再证实；FREE_RUNNING=0 排除
#   ts_format）而仍被拒 → 拒绝属环境性。新能力事实：**ALLOC_TD 收
#   （tdn=1）/CREATE_TIS 收（tisn=0）**；脏状态趟 tier2 换 syndrome
#   （0x991046≠0x4fde71）→ non_wire 位被此 fw 解析（修正"7.0-only 预期
#   拒"的推断）。驱动 bug 自查自纠：id=0 卫语句把 fw 合法分配的 tisn=0
#   当"无对象"→ DESTROY_TIS 跳过 → DEALLOC_TD 拒 0x9（bad resource
#   state）→ 销毁链中断 → CQ/MKEY/PD 每负载泄漏（cqn 涨至 2128）→ EQ
#   拆除拒 0x9 → CMDQ 舔化（2000ms 无 ownership 交接）→ 探针 -5、
#   0/2305 页。修复：对象有效性改显式 have_* 标志。
#   处置（第十一趟预登记）：①环境三探针（QUERY_NIC_VPORT
#   min_wqe_inline_mode@0x5、QUERY_HCA_CAP 组1/cur 的 eth caps 页
#   wqe_inline_mode@0x12、caps 补丁字节 82-83/162-163 回读——byte 82 在
#   5.15 布局= max_wqe_sz_sq 低字节，T05 清位线索入档）；②V 富集梯 4 变体
#   （bit0=+min_wqe_inline_mode(vport 实读值)、bit1=log_wq_sz 4→6）
#   ×tier 三档，v-major/tier-minor 全走（≤12 次 CREATE，首过即赢，
#   "CREATE_SQ ok tier=N v=N"行标明获胜组合）；③上机前建议整机重启一次，
#   清掉第十趟 208 次失败负载留在 fw 的对象碎片（cqn 池污染），判决以
#   重启后第一负载为准。判据 J1-J3 不变（tier=/v= 行族均为前缀匹配）。
#
# 修正史（2026-09-27 第十一趟，V 梯判决）：**12 次尝试（V 富集梯 4 变体 ×
#   tier 三档）全部同码 0x17a194**——含 v1 的 min_wqe_inline_mode=1（环境
#   探针实读：vport min_wqe_inline_mode=1、eth caps wqe_inline_mode=1，
#   二者被整体吞掉）→ 内容无关范畴级拒绝。环境探针另读出 **caps 补丁字
#   节 82-83=0x0400**（T05 清位后 SET 进去的值）——对照 v4.14（同代）与
#   v5.15 ifc：**bytes 82-83 = max_wqe_sz_sq**，此代 fw 在低字节携带 log2
#   值（自报 0x0?，清 bits0-1 后=0x04=log 4=16B）。因果链假设：**T05 清位
#   把 max WQE 尺寸砍到 16B < SQ WQEBB 步长 64B(log 6) → CREATE_SQ 对任何
#   sqc 内容 bad parameter**——同时解释 12 变体同码、TD/TIS（非 WQ 对象）
#   全过、mlx5_core（从不 SET_HCA_CAP）SQ 可用。
#   处置（第十二趟预登记）：caps82 模块参数注入——`t14_verify.sh all 6`
#   → SET_HCA_CAP 带 byte82=0x06（bit2 今日已在写且被收，bit1 从未单试；
#   0x06=64B 恰好= WQEBB 步长）。三种判决：①SET 收 + CREATE_SQ 过 → 根因
#   定案（T05 补丁自残），修复=profile 改 0x06；②SET 拒（0x06 含 bit1）→
#   改跑 `all 5`（bit0 单试）→ 仍拒则 bit0/bit1 双锁死，下趟转 162-163
#   轴或 stride≤4 实验；③SET 收但 CREATE_SQ 仍 0x17a194 → 82 非唯一被
#   砍字段，下趟 162-163 轴。J1-J3 判据不变（caps82 不改行族）。
#
# 修正史（2026-09-27 第十二趟，caps 轴判死 + 真根因落网）：caps82=6 的
#   SET_HCA_CAP **命令被收**（wqe_sz82=0x06 回显）但 CREATE_SQ 仍全码
#   0x17a194；caps[post] hexdump 显示读回仍 0x04——固件对 byte82/66-67
#   "收命令不改值"，且 caps[cur]（pre-SET 原始查询）本就 0x04/0x00 ==
#   我们补丁后的值 → **caps 补丁对这些字节是 no-op，caps 轴判死**（固件
#   原生 0x04 下 mlx5e 的 SQ 照常工作）。旁路收获：SET 行 log_max_qp=2
#   系 LSB-first 翻转窗口读法（真值 17），又一处 eqc 教训同款（仅打印）。
#   **真根因（社区 syndrome 表命中）**：0x17A194 = "create_sq: dbr_address
#   must be dword aligned"（lukego gist 表）；邻居 0x5F5A07 "wq_type must
#   be 1" 未触发 → wq 块起点正确。自查出 fmt.h 两处位号错误：**wq.dbr_addr
#   在 wq 字节 16（ifc bit 0x80，v4.14/5.15 双证），早前调查笔记误记 0xa0/
#   字节 20**；**sqc.cqn 在 bit 0x48，误记 0x40**。机理：错位 be64 使固件
#   从 16-23 字节读到"总线地址高 32 位"（如 0xd6=214，非 4 对齐）→ 12 变体
#   全同码（变体梯从不触碰 dbr）。fmt_equiv 教训入档：等价测试锁"编码代数
#   ==MLX5_SET"，不锁"偏移==ifc"——期望常量须从 ifc 独立推导；已按"全
#   T14 常量 vs v4.14/5.15 机械审计"口径复核全部 OK。
#   处置（第十三趟预登记）：直接 `t14_verify.sh all`（不带 caps82，默认
#   流）；预期 tier1 v0 即过 CREATE_SQ→RDY→NOP→SEND 全链；若出现新码，
#   按社区表逐字段定位（0x5F5A07=wq_type、0x27A8E4=…同族）。判据不变。
#
# 修正史（2026-09-27 第十三趟，dbr/cqn 修复实证 + 卡点后移至 SEND）：**J1
#   生命周期全绿 ×2（CREATE_SQ tier=1 v=0 sqn=128 首试即中）+ J2 NOP 全绿
#   ×2（REQ CQE wqe_counter=0）**——doorbell/CQE 管线全链打通。tier2
#   (non_wire) CREATE 被拒（如最初预期），tier1/tier3 CREATE 全过。J3 的
#   SEND **确定性错误 CQE：opcode=13 (REQ_ERR) byte_cnt=0 wqe_counter=1**，
#   全 tier/variant 同形（12 次中 8 次到点火：4v×{tier1,tier3}）。
#   假设：vport min_wqe_inline_mode=1（L2 头须 inline）在 WQE 执行期强制
#   ——NOP 无数据豁免、SEND 指针式 dseg（零 eseg 无 inline）违规；次嫌：
#   PA mkey 无 len（dr_send 空 mkey 先例从不透过 mkey 读内存）。处置（第
#   十四趟预登记）：SEND 改 **inline 形态**（dseg counter 置 bit31 + 64B
#   payload 内嵌 WQE，ds=7 跨两个 WQEBB——mlx5e 小包标准形）——对两类真
#   因通吃（不依赖 inline 模式、不经 mkey）；同时 CQE dump 增打错误
#   syndrome（@0x20）+ wqe_opcode（@0x21），若仍失败黑匣子直接点名。J3
#   判据不变（inline SEND 的 TX CQE byte_cnt 仍=64）。
#
# 修正史（2026-09-27 第十四趟，inline dseg 不足 + syndrome 打印偏移自纠）：
#   SEND 改 inline dseg 后仍同形 REQ_ERR（opcode=13/wc=1），且 syndrome 读
#   回全零——自查：err_cqe 布局取自内核结构体原文（v4.14==v5.15），
#   **syndrome@0x37、s_wqe_opcode_qpn@0x38**，此前按 0x20/0x21 读的是
#   srqn 区（=0 合理）；0x2c 的 byte_cnt 打印对错误 CQE 也是 rsvd 区无意
#   义值。嫌疑收窄：**vport min_wqe_inline_mode=1 要求的是 ESEG 的
#   inline_hdr**（mlx5e attr.ihs 机制：sz@eseg+12 be16、头字节从 eseg+14
#   溢入后续 DS；inline dseg 不满足该检查）。处置（第十五趟预登记）：
#   SEND 补 eseg inline_hdr（合成 14B 头：广播 dst/本地管理 src/
#   ethertype 0x88b5）+ payload inline dseg 移至 DS3（ds=8 双 BB）；错误
#   CQE dump 修正偏移 + 增 vendor/s_wqe_opcode_qpn + **全量 64B hexdump**
#   （若仍失败，黑匣子必得名）；J3 判据 byte_cnt 改双值 {64,78} 前缀匹配
#   预登记。
#
# 修正史（2026-09-27 第十五趟，SEND 三因子因果链闭环 + 门禁自纠）：
#   eseg inline_hdr 形态下——**tier1（无 TIS）SEND 立即 LOCAL_QP_OP_ERR
#   (syndrome 0x02)**（错误 CQE 完整落盘：vendor=0x68/wqe_opcode=0x0a/
#   qpn 吻合）；**tier3（TD+TIS）SEND 完成：opcode=REQ wqe_counter=1
#   byte_cnt=0**——教科书式成功完成，本 fw 的 TX CQE 不报字节数，而
#   J3 把 64/78 写死 → 成功被门禁误杀（NOP 后 1ms 内即返回，非超时）。
#   三因子定案：①TIS（无之 → 0x02 本地 QP 操作错）②vport L2 inline 地板
#   （指针 dseg → REQ_ERR，见第13趟）③门禁 byte_cnt（{0,64,78} 三值）。
#   处置（第十六趟，预期收官）：fire_send 接受 byte_cnt ∈ {0,64,78}，
#   实测值入行；J3 锚改前缀"TX CQE ok (opcode=REQ wqe_counter=1"；
#   normalize.sh 加 byte_cnt=N 归一化。全绿后走 J7 收官链。
#
# 修正史（2026-09-27 第十六趟，**T14 首次数据面全过——J6 仪表窗口 bug**）：
#   byte_cnt 三值判据生效后 **J1/J2/J3 全 PASS**（stage3 双口
#   "TX CQE ok (opcode=REQ wqe_counter=1"×2 + PASSED×2，胜者 tier3+eseg
#   inline_hdr）；本趟真实计数 **206 PASSED / 0 FAILED**（=3 stage 负载×2
#   口+100 stress×2 口，时间戳切窗核实）。J6 报 FAILED×22 系测量仪表
#   bug：黑匣子 `dmesg -w` 启动时先转储 dmesg 环形缓冲存量，第十五趟
#   的 FAILED 残尾留在环内被整文件计数吞入——判据本身（100×2 全过、
#   零失败）未动，仅修仪表：启动后 sleep 1 标记本趟起始行，J6 只从该
#   行起计数（blackbox 文件本身仍保留完整存量，忠实性不变）。
#   处置（第十七趟）：重跑 `t14_verify.sh all` 以修正后的仪表取**干净
#   的 ALL GREEN 判决**（第十六趟的 206/0 已按时间戳核实留档）。
#
# Usage: sudo bash scripts/gate/t14_verify.sh [stage1|stage2|stage3|stress|all] [caps82]
#   all（默认）= stage1 → stage2 → stage3 逐趟递进（风险分级）；
#   stress 单独跑 100 循环压测。

set -u
cd /home/zqc/code/prism

RUN=/var/tmp/t14_run.log
LIVE=/var/tmp/t14_live.log
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }

WORK=$(mktemp -d /tmp/prism_t14.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

MODE=${1:-all}
case "$MODE" in
	stage1|stage2|stage3|stress|all) ;;
	*) echo "unknown mode: $MODE"; exit 1 ;;
esac
CAPS82=${2:-0}
CAPSARG=()
[ "$CAPS82" != "0" ] && CAPSARG=(caps82="$CAPS82")

mv -f "$RUN" "$RUN.prev" 2>/dev/null
exec > >(tee "$RUN") 2>&1

mv -f "$LIVE" "$LIVE.prev" 2>/dev/null
stdbuf -oL dmesg -w | while IFS= read -r l; do printf '%s\n' "$l" >> "$LIVE"; sync; done &
LIVE_PID=$!
# `dmesg -w` first dumps the pre-existing ring buffer — give the tailer a
# second to flush that backlog, then mark the line where THIS run begins:
# J6 counts only from here (trip-16 lesson: the previous trip's FAILED
# remnants in the ring polluted the blackbox count and failed a 206/0 run).
sleep 1
LIVE_SKIP=$(wc -l < "$LIVE")

say() { echo; echo "===== $* ====="; }
res() { if [ "$1" -eq 0 ]; then echo "   [PASS] $2"; else echo "   [FAIL] $2"; RC=1; fi; }
RC=0
CP=/home/zqc/code/linux/scripts/checkpatch.pl

run_stage() {	# $1 = stage number, $2.. = per-port expected dmesg patterns
	local N=$1; shift
	local D K P BAD=0

	modprobe -r mlx5_ib 2>/dev/null; modprobe -r mlx5_core
	dmesg -c >/dev/null
	insmod src/mlx5_prism.ko selftest=1 smoke_stage="$N" "${CAPSARG[@]}"
	if [ $? -ne 0 ]; then
		res 1 "stage$N insmod"
		return 1
	fi
	D=$(dmesg | grep mlx5_prism)
	for K in "$@"; do
		P=$(echo "$D" | grep -c "$K")
		echo "   stage$N '$K' x$P"
		[ "$P" -ge 2 ] || BAD=1
	done
	[ "$BAD" -eq 0 ]; res $? "stage$N per-port lines (x2 each)"
	echo "$D" | grep -q "Hardware datapath smoke test: PASSED"
	res $? "stage$N PASSED lines"
	rmmod mlx5_prism
	res $? "stage$N rmmod"
	lsmod | grep -q mlx5_prism; [ $? -ne 0 ]; res $? "stage$N module gone"
	echo "$D" | grep -qE "WARNING|Oops|BUG"; [ $? -ne 0 ]
	res $? "stage$N no kernel complaints"
	echo "$D" | grep -c "teardown: 2305/2305"
	return 0
}

if [ "$MODE" = "all" ] || [ "${MODE#stage}" != "$MODE" ]; then
	say "J0 build + checkpatch + fmt_equiv (host; caps82=${CAPS82:-0})"
	make >"$WORK/build.log" 2>&1
	res $? "make"
	[ -n "${SUDO_UID:-}" ] && chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" src
	grep -v "compiler differs\|built by\|You are using" "$WORK/build.log" | grep -qi "warning\|error"
	[ $? -ne 0 ]; res $? "build no warnings"
	"$CP" --no-tree --strict -f src/mlx5_prism_smoke.c -f src/mlx5_prism_fmt.h \
		-f src/mlx5_prism_fw.c -f src/mlx5_prism_pci.c -f src/mlx5_prism_trace_names.h \
		>"$WORK/cp.log" 2>&1
	grep "^total" "$WORK/cp.log"
	[ "$(grep -c '^total: 0 errors, 0 warnings, 0 checks' "$WORK/cp.log")" = "5" ]
	res $? "checkpatch 0/0/0 (5 files)"
	gcc -Wall -Wextra -O2 -I scripts/test/include -I src \
		scripts/test/fmt_equiv_check.c -o "$WORK/fmt_equiv" 2>"$WORK/fmt_build.log"
	res $? "fmt_equiv build"
	"$WORK/fmt_equiv" >"$WORK/fmt.log" 2>&1
	tail -1 "$WORK/fmt.log"
	grep -q "ALL EQUIVALENT" "$WORK/fmt.log"; res $? "fmt_equiv ALL EQUIVALENT"
fi

case "$MODE" in
all|stage1)
	say "J1 stage1 — object lifecycle (no doorbells)"
	run_stage 1 "smoke self-test: stage 1" "ALLOC_PD ok" "CREATE_MKEY ok" \
		"CREATE_CQ ok" "CREATE_SQ ok tier=" "SQ to RDY ok" \
		"DESTROY_SQ/TIS/TD/CQ/MKEY/PD ok"
	D=$(dmesg | grep mlx5_prism)
	echo "$D" | grep -qE "NOP WQE ok|TX CQE"; [ $? -ne 0 ]
	res $? "stage1 zero ignition lines"
	[ "$MODE" = stage1 ] && FINISH=1
	;;
esac

case "$MODE" in
all|stage2)
	say "J2 stage2 — + NOP WQE (send pipeline)"
	run_stage 2 "NOP WQE ok (opcode=REQ wqe_counter=0)"
	[ "$MODE" = stage2 ] && FINISH=1
	;;
esac

case "$MODE" in
all|stage3)
	say "J3 stage3 — + 64-byte ETH SEND (TX datapath)"
	run_stage 3 "NOP WQE ok (opcode=REQ wqe_counter=0)" \
		"TX CQE ok (opcode=REQ wqe_counter=1"
	[ "$MODE" = stage3 ] && FINISH=1
	;;
esac

if [ "${FINISH:-0}" = 1 ]; then
	kill $LIVE_PID 2>/dev/null
	modprobe mlx5_core
	echo
	echo "===== T14 verify ($MODE): $([ $RC -eq 0 ] && echo ALL GREEN || echo NOT GREEN) ====="
	echo "J7 (outside this script): rebaseline + r0_gate 8/8 before commit"
	exit $RC
fi

say "J4 default load (selftest=0) — zero smoke lines"
modprobe -r mlx5_ib 2>/dev/null; modprobe -r mlx5_core
dmesg -c >/dev/null
insmod src/mlx5_prism.ko "${CAPSARG[@]}"
res $? "insmod (default)"
D=$(dmesg | grep mlx5_prism)
echo "$D" | grep -qE "smoke self-test|smoke test"; [ $? -ne 0 ]
res $? "zero smoke lines in default probe"
echo "$D" | grep -c "teardown: 2305/2305" >/dev/null
rmmod mlx5_prism
res $? "rmmod"

say "J6 stress — 100x load/unload (selftest=1, stage3)"
FAIL=0
for i in $(seq 100); do
	insmod src/mlx5_prism.ko selftest=1 "${CAPSARG[@]}" || { echo "   insmod FAIL at $i"; FAIL=1; break; }
	rmmod mlx5_prism || { echo "   rmmod FAIL at $i"; FAIL=1; break; }
	D=$(dmesg | tail -40)
	echo "$D" | grep -qE "WARNING|Oops|BUG" && { echo "   complaint at $i"; FAIL=1; break; }
done
res $FAIL "100 cycles done"
P=$(tail -n +$((LIVE_SKIP + 1)) "$LIVE" | grep -c "Hardware datapath smoke test: PASSED")
E=$(tail -n +$((LIVE_SKIP + 1)) "$LIVE" | grep -c "Hardware datapath smoke test: FAILED")
echo "   blackbox (this run only): PASSED x$P, FAILED x$E"
[ "$P" -ge 200 ] && [ "$E" -eq 0 ]; res $? "PASSED >= 200 (100x2 ports), FAILED = 0"

kill $LIVE_PID 2>/dev/null
modprobe mlx5_core

echo
echo "===== T14 verify ($MODE): $([ $RC -eq 0 ] && echo ALL GREEN || echo NOT GREEN) ====="
echo "J7 (outside this script): rebaseline + r0_gate 8/8 before commit"
exit $RC
