#!/bin/sh
# 一鍵啟動 PWM 網頁示範(root 執行):
#   1) 確保 R8 跑的是 cr8_pwm_demo.elf(不是就用 start-cr8.sh 換,含 MHU 清理)
#   2) (重)啟網頁服務 http://<板子IP>:8080/
# 輸出腳位:GPT6 → GTIOC6A = P76、GTIOC6B = P77(示範韌體把羅盤 I2C 的腳還給了 GPT6)
set -u

FW=${FW:-cr8_pwm_demo.elf}
DEVKIT=${DEVKIT:-/home/ubuntu/devkit}
R8WEB=${R8WEB:-/home/ubuntu/r8web}
RP=/sys/class/remoteproc/remoteproc1

STATE=$(cat $RP/state 2>/dev/null || echo none)
CUR=$(cat $RP/firmware 2>/dev/null || echo none)
if [ "$STATE" != "running" ] || [ "$CUR" != "$FW" ]; then
    [ -f /lib/firmware/$FW ] || { echo "缺 /lib/firmware/$FW"; exit 1; }
    "$DEVKIT/tools/start-cr8.sh" "$FW" || exit 1
else
    echo "R8 已在跑 $FW"
fi

exec "$R8WEB/restart-pwm-web.sh"
