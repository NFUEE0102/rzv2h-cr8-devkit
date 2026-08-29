#!/bin/sh
# mine #5 的正規重啟程序:CR8 無 per-core reset —— 另核 running 時 stop 單核
# 只是 halt(不 reset),再 start 會從舊 PC 續跑撞新 image = 跑飛。
# 換任一 R8 韌體 / 重啟任一 R8 → 一律用本腳本:兩核全下 → core0 → core1。
# 用法:sudo restart-r8-pair.sh [core0.elf] [core1.elf](預設現行 demo 對)
set -u
FW0=${1:-cr8_pwm_demo.elf}
FW1=${2:-cr8_core1_demo.elf}

echo "== 兩核全下(cluster 完整 reset 的前提)=="
echo stop > /sys/class/remoteproc/remoteproc2/state 2>/dev/null
sleep 2
echo stop > /sys/class/remoteproc/remoteproc1/state 2>/dev/null
sleep 2

echo "== 依序上 =="
FORCE=1 /home/ubuntu/devkit/tools/start-cr8.sh "$FW0" 2>/dev/null | tail -1
FORCE=1 /home/ubuntu/devkit/tools/start-cr8-core1.sh "$FW1" 2>/dev/null | tail -1
sleep 2
echo "== 黑盒 =="
python3 /home/ubuntu/r8web/bb.py --all 2>/dev/null
