#!/bin/sh
# 換 CM33 韌體的完整安全程序(root)—— docs/11 地雷 #4 的腳本化:
# stop CM33 會 memset 63MB carveout(抹掉 core0 全部 + core1 rsctbl),
# 所以換裝後必須把兩顆 R8「全下再依序上」重建。
# 用法:redeploy-cm33.sh <新elf路徑> [安裝名,預設 cm33_demo.elf]
set -u
NEW=${1:?usage: redeploy-cm33.sh <new.elf> [installed-name]}
NAME=${2:-cm33_demo.elf}
[ -f "$NEW" ] || { echo "no such file: $NEW"; exit 1; }

echo "== 1) stop CM33(此步會抹 0x40010000-0x43F0FFFF —— 兩顆 R8 隨後重建)=="
echo stop > /sys/class/remoteproc/remoteproc0/state 2>/dev/null
sleep 2
[ "$(cat /sys/class/remoteproc/remoteproc0/state)" = "offline" ] || { echo "CM33 不肯停"; exit 1; }

echo "== 2) 安裝 + start 新 CM33 =="
[ -f "/lib/firmware/$NAME" ] && cp "/lib/firmware/$NAME" "/lib/firmware/$NAME.bak-$(date +%Y%m%d-%H%M%S)"
cp "$NEW" "/lib/firmware/$NAME"
/home/ubuntu/devkit/tools/start-cm33.sh "$NAME" || exit 1

echo "== 3) 兩顆 R8 全下 =="
echo stop > /sys/class/remoteproc/remoteproc2/state 2>/dev/null
sleep 2
echo stop > /sys/class/remoteproc/remoteproc1/state 2>/dev/null
sleep 2

echo "== 4) R8 依序上 =="
/home/ubuntu/devkit/tools/start-cr8.sh cr8_pwm_demo.elf 2>/dev/null | tail -1
/home/ubuntu/devkit/tools/start-cr8-core1.sh cr8_core1_demo.elf 2>/dev/null | tail -1
sleep 2
echo "== 5) 三核狀態 =="
for d in /sys/class/remoteproc/remoteproc0 /sys/class/remoteproc/remoteproc1 /sys/class/remoteproc/remoteproc2; do
    echo "  $(cat $d/name) = $(cat $d/state)"
done
