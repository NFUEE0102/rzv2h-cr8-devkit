#!/bin/sh
# 乾淨重啟 PWM 網頁服務(root 執行)。
# SIGINT server → server 關 daemon stdin → daemon EOF 收攤(絕不 SIGKILL)。
set -u

PID=$(ps -eo pid,args | grep "[s]erve_pwm.py" | awk '{print $1}')
if [ -n "$PID" ]; then
    echo "停舊 server pid=$PID"
    kill -INT $PID
    for i in 1 2 3 4 5 6 7 8; do
        ps -p $PID >/dev/null 2>&1 || break
        sleep 1
    done
    # 舊版 server 若是背景 sh 啟動的,SIGINT 被繼承成 ignore —— 補 SIGTERM。
    # (新版 serve_pwm.py 兩個訊號都走乾淨收攤;daemon 靠 stdin EOF 自理。)
    if ps -p $PID >/dev/null 2>&1; then
        echo "SIGINT 無效,改 SIGTERM"
        kill -TERM $PID
        for i in 1 2 3 4 5 6 7 8; do
            ps -p $PID >/dev/null 2>&1 || break
            sleep 1
        done
    fi
    ps -p $PID >/dev/null 2>&1 && { echo "server 沒退,放棄(不硬殺)"; exit 1; }
fi

if ps -eo args | grep -q "[p]wmd"; then
    echo "⚠ daemon 還活著(server 沒帶走它?),等 5 秒"
    sleep 5
    ps -eo args | grep "[p]wmd" && { echo "daemon 仍在,不硬殺,人工處理"; exit 1; }
fi

cd /home/ubuntu/r8web
nohup python3 -u serve_pwm.py > /run/r8web-pwm.log 2>&1 &
sleep 4
echo "--- /run/r8web-pwm.log ---"
head -5 /run/r8web-pwm.log
