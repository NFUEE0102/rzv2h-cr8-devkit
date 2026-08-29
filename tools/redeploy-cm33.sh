#!/bin/sh
# 換 CM33 韌體(root)。
# 本映像的 DTB 把 CM33 stop 的清除範圍限制在自家三個窗(sram/rsctbl/vring),
# stop CM33 不影響 R8。本腳本換裝後仍驗證兩顆 R8 完好;
# 在未分窗的舊 DTB 上驗證失敗時,自動執行 R8 全下重建。
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

echo "== 3) 驗證兩顆 R8 完好(窗已修 = 應毫髮無傷)=="
ok=1
for a in 0x439F0000 0x441F0000; do
    v=$(busybox devmem $a 32 2>/dev/null)
    [ "$v" = "0x42423852" ] || { echo "  ✗ $a magic=$v —— R8 黑盒被抹(舊 DTB?)"; ok=0; }
done
if [ $ok = 0 ]; then
    echo "== 3b) 舊世界復原:兩顆 R8 全下再依序上 =="
    /home/ubuntu/devkit/tools/restart-r8-pair.sh 2>/dev/null | tail -3
else
    echo "  ✓ R8 完好,無需重建"
fi
echo "== 4) 三核狀態 =="
for d in /sys/class/remoteproc/remoteproc0 /sys/class/remoteproc/remoteproc1 /sys/class/remoteproc/remoteproc2; do
    echo "  $(cat $d/name) = $(cat $d/state)"
done
