#!/bin/sh
# per-core carveout 板側改造(root 執行):
#   1) DTB 加 8 個 per-core vring/shm UIO 節點(core0 @43[89AD]xxxxx、core1 @44xxxxxx)
#   2) cr8-uio-bind.sh PATTERNS 加 10 條(含既存的 rsctbl-cr8-0/1)
#   3) start-cr8.sh 的 virtio status 清理改成新舊 rsctbl 都清(通吃兩種佈局)
# 全部冪等;DTB 先備份 .pre-percore。改完需重開機生效。
set -u
DTB=/boot/r9a09g057h44-rzv2h-evk.dtb
BIND=/usr/local/sbin/cr8-uio-bind.sh

echo "== 1) DTB =="
[ -f "$DTB.pre-percore" ] || cp "$DTB" "$DTB.pre-percore"
add_node() {   # add_node <節點名> <base_hex> <size_hex>
    n="/$1"
    if fdtget "$DTB" "$n" compatible >/dev/null 2>&1; then
        echo "  已存在 $1"
        return
    fi
    fdtput -c "$DTB" "$n" || { echo "  ✗ 建節點失敗 $1"; exit 1; }
    fdtput -t s "$DTB" "$n" compatible "$4"
    fdtput -t x "$DTB" "$n" reg 0x0 "$2" 0x0 "$3"
    echo "  + $1 ($4 @$2 len $3)"
}
# core0
add_node "vring-ctl0-c0@43800000" 0x43800000 0x100000 vring_uio
add_node "vring-shm0-c0@43a00000" 0x43a00000 0x300000 shm_uio
add_node "vring-ctl1-c0@43900000" 0x43900000 0x100000 vring_uio
add_node "vring-shm1-c0@43d00000" 0x43d00000 0x300000 shm_uio
# core1(Phase 3 用,順手備妥)
add_node "vring-ctl0-c1@44000000" 0x44000000 0x100000 vring_uio
add_node "vring-shm0-c1@44200000" 0x44200000 0x300000 shm_uio
add_node "vring-ctl1-c1@44100000" 0x44100000 0x100000 vring_uio
add_node "vring-shm1-c1@44500000" 0x44500000 0x300000 shm_uio

echo "== 2) bind PATTERNS =="
if grep -q "vring-ctl0-c0" "$BIND"; then
    echo "  已含新 pattern"
else
    cp "$BIND" "$BIND.pre-percore"
    # 插在既有清單尾(')'那行前),舊節點順序不動 → uio0-9 編號不變
    awk "/^\)/ && !done { print \"    # per-core carveout(2026-08-29):rsctbl 與 vring 視窗\"; \
print \"    '*.rsctbl-cr8-0'\"; print \"    '*.rsctbl-cr8-1'\"; \
print \"    '*.vring-ctl0-c0'\"; print \"    '*.vring-shm0-c0'\"; \
print \"    '*.vring-ctl1-c0'\"; print \"    '*.vring-shm1-c0'\"; \
print \"    '*.vring-ctl0-c1'\"; print \"    '*.vring-shm0-c1'\"; \
print \"    '*.vring-ctl1-c1'\"; print \"    '*.vring-shm1-c1'\"; done=1 } { print }" \
        "$BIND" > "$BIND.tmp" && mv "$BIND.tmp" "$BIND" && chmod 755 "$BIND"
    echo "  已加 10 條 pattern"
fi

echo "== 3) start-cr8.sh 換雙佈局版(/tmp/start-cr8.sh 由 scp 先送到)=="
S=/home/ubuntu/devkit/tools/start-cr8.sh
if [ -f /tmp/start-cr8.sh ]; then
    grep -q "BB_PERCORE" "$S" 2>/dev/null || cp "$S" "$S.pre-percore"
    install -m 755 /tmp/start-cr8.sh "$S"
    echo "  已換版(備份 $S.pre-percore)"
else
    echo "  ⚠ /tmp/start-cr8.sh 不在,略過"
fi

echo "== 驗證 DTB =="
fdtget "$DTB" /vring-ctl0-c0@43800000 compatible && fdtget -t x "$DTB" /vring-ctl0-c0@43800000 reg
echo "DONE — 需重開機讓新節點生效"
