#!/bin/bash
# ===========================================================================
# Lab1 数据链路层 Go-Back-N 滑动窗口协议 — 验收测试脚本
# ===========================================================================
# 用法:
#   手动启动 (推荐):  阅读本脚本, 按顺序在两个终端中执行对应的命令
#   快速自测:          ./run_acceptance_tests.sh quick
# ===========================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

BIN="./datalink"

if [ ! -x "$BIN" ]; then
    echo "[ERROR] 请先执行 make 编译项目"
    exit 1
fi

# ---- 快速自测 (短时间) ----
do_quick() {
    echo "============================================"
    echo "  快速自测 (短测不替代表 3 正式 20 分钟测试)"
    echo "============================================"

    echo ""
    echo ">>> 理想信道短测 (30s)"
    echo "    A端:  stdbuf -oL $BIN -u -d3 -t30 A  2>&1 | tee /tmp/quick-ideal-A.log"
    echo "    B端:  stdbuf -oL $BIN -u -d3 -t30 B  2>&1 | tee /tmp/quick-ideal-B.log"
    echo ""

    echo ">>> 差错信道短测 BER=1e-3 (30s)"
    echo "    A端:  stdbuf -oL $BIN -b1e-3 -d3 -t30 A  2>&1 | tee /tmp/quick-ber-A.log"
    echo "    B端:  stdbuf -oL $BIN -b1e-3 -d3 -t30 B  2>&1 | tee /tmp/quick-ber-B.log"
    echo ""

    echo ">>> 洪水模式短测 (20s)"
    echo "    A端:  stdbuf -oL $BIN -u -f -d1 -t20 A  2>&1 | tee /tmp/quick-flood-A.log"
    echo "    B端:  stdbuf -oL $BIN -u -f -d1 -t20 B  2>&1 | tee /tmp/quick-flood-B.log"
    echo ""
}

# ---- 表 3 正式测试命令 ----
do_formal() {
    echo "============================================"
    echo "  表 3 正式性能测试 (每组稳定运行 20 分钟)"
    echo "============================================"
    echo ""
    echo "  每组的 B 端必须在 A 端启动并监听到端口后再启动。"
    echo ""

    echo "------------------------------------------------------"
    echo "  第 1 组: 无误码信道普通传输"
    echo "------------------------------------------------------"
    echo "    A端:  stdbuf -oL $BIN -u -d1 -t1200 A  2>&1 | tee /tmp/t3-g1-A.log"
    echo "    B端:  stdbuf -oL $BIN -u -d1 -t1200 B  2>&1 | tee /tmp/t3-g1-B.log"
    echo ""

    echo "------------------------------------------------------"
    echo "  第 2 组: 默认误码率普通传输"
    echo "------------------------------------------------------"
    echo "    A端:  stdbuf -oL $BIN -d1 -t1200 A  2>&1 | tee /tmp/t3-g2-A.log"
    echo "    B端:  stdbuf -oL $BIN -d1 -t1200 B  2>&1 | tee /tmp/t3-g2-B.log"
    echo ""

    echo "------------------------------------------------------"
    echo "  第 3 组: 无误码信道 + A/B 双端洪水模式"
    echo "------------------------------------------------------"
    echo "    A端:  stdbuf -oL $BIN -u -f -d1 -t1200 A  2>&1 | tee /tmp/t3-g3-A.log"
    echo "    B端:  stdbuf -oL $BIN -u -f -d1 -t1200 B  2>&1 | tee /tmp/t3-g3-B.log"
    echo ""

    echo "------------------------------------------------------"
    echo "  第 4 组: 默认误码率 + A/B 双端洪水模式"
    echo "------------------------------------------------------"
    echo "    A端:  stdbuf -oL $BIN -f -d1 -t1200 A  2>&1 | tee /tmp/t3-g4-A.log"
    echo "    B端:  stdbuf -oL $BIN -f -d1 -t1200 B  2>&1 | tee /tmp/t3-g4-B.log"
    echo ""

    echo "------------------------------------------------------"
    echo "  第 5 组: 误码率 1e-4 + A/B 双端洪水模式"
    echo "------------------------------------------------------"
    echo "    A端:  stdbuf -oL $BIN -f -b1e-4 -d1 -t1200 A  2>&1 | tee /tmp/t3-g5-A.log"
    echo "    B端:  stdbuf -oL $BIN -f -b1e-4 -d1 -t1200 B  2>&1 | tee /tmp/t3-g5-B.log"
    echo ""
}

# ---- 测试通过判据 ----
do_criteria() {
    echo "============================================"
    echo "  测试通过判据"
    echo "============================================"
    echo ""
    echo "1. 程序持续输出: .... xxx packets received, xxxx bps, xx.xx%, Err xx (...)  "
    echo "2. A/B 两端都正常 Quit，无异常退出。"
    echo "3. 无误码信道中 Err 应为 0。"
    echo "4. 有误码信道中允许出现 Bad CRC Checksum 和 DATA timeout。"
    echo "5. Go-Back-N 关键证据: timeout 后从 ack_expected 开始连续重传窗口内所有帧。"
    echo "6. 洪水模式下高负载时不出现死锁、窗口受限、利用率稳定。"
    echo "7. 不出现以下错误信息:"
    echo "   - Network Layer received a bad packet from data link layer"
    echo "   - Network Layer: incorrect packet length"
    echo "   - Physical Layer Sending Queue overflow"
    echo "   - Memory used by 'protocol.lib' is corrupted by your program"
    echo ""
}

# ----
case "${1:-}" in
    quick)
        do_quick
        ;;
    formal)
        do_formal
        do_criteria
        ;;
    *)
        do_quick
        echo ""
        do_formal
        do_criteria
        ;;
esac
