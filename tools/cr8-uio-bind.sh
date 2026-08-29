#!/bin/bash
# cr8-uio-bind: 把 CR8 rpmsg 傳輸層需要的 platform device 綁到 uio_pdrv_genirq。
#
# 這些節點的 compatible(vring_uio / shm_uio / mbox_uio / receiver)在整個 kernel 樹
# 裡沒有任何 driver 的 of_match,綁定是執行期用 sysfs driver_override + bind 完成的
# (doc 13 §7.6)。所以開機後 /dev/uio* 是空的,要先跑這支。
#
# 現在:從 sysfs 列舉(不再硬寫大小寫,DT 改名也不會再咬人)、綁齊、逐項回報、
# 有任何一項失敗就回非 0。
#
# 註:libmetal 是用 metal_device_open(bus_name, name, ...) **按名稱**查找
# (rz_rproc.c:132、457),不是按 /dev/uioN 索引,所以綁定順序不影響功能 ——
# 下面的順序只是讓 uio 編號穩定、log 好讀。

set -u

DRV=/sys/bus/platform/drivers/uio_pdrv_genirq
DEVDIR=/sys/bus/platform/devices

# glob,不是完整名稱 —— 位址前綴與大小寫都交給 sysfs 決定。
# 註:'*.rsctbl' 只會命中 42f00000.rsctbl,不會命中 42f02000.rsctbl-cr8-0(結尾不同)。
PATTERNS=(
    '*.mbox-uio'
    'receiver@*'
    '*.rsctbl'
    '*.mhu-shm'
    '*.vring-ctl0'
    '*.vring-shm0'
    '*.vring-ctl1'
    '*.vring-shm1'
)

[ "$(id -u)" -eq 0 ] || { echo "cr8-uio-bind: 需要 root" >&2; exit 1; }
[ -d "$DRV" ] || { echo "cr8-uio-bind: uio_pdrv_genirq 未載入 —— 先 modprobe uio_pdrv_genirq" >&2; exit 1; }

bound=0; already=0; failed=0

for pat in "${PATTERNS[@]}"; do
    hit=0
    for path in "$DEVDIR"/$pat; do
        [ -e "$path" ] || continue
        hit=1
        d=${path##*/}

        if [ -d "$DEVDIR/$d/uio" ]; then
            u=$(ls "$DEVDIR/$d/uio" 2>/dev/null | head -1)
            printf '  already  %-24s %s\n' "$d" "${u:-uio?}"
            already=$((already + 1))
            continue
        fi

        echo uio_pdrv_genirq > "$DEVDIR/$d/driver_override" 2>/dev/null
        echo "$d" > "$DRV/bind" 2>/dev/null

        if [ -d "$DEVDIR/$d/uio" ]; then
            u=$(ls "$DEVDIR/$d/uio" 2>/dev/null | head -1)
            printf '  bound    %-24s %s\n' "$d" "${u:-uio?}"
            bound=$((bound + 1))
        else
            printf '  FAILED   %-24s\n' "$d" >&2
            failed=$((failed + 1))
        fi
    done
    if [ "$hit" -eq 0 ]; then
        printf '  MISSING  no device matches %s\n' "$pat" >&2
        failed=$((failed + 1))
    fi
done

echo "cr8-uio-bind: bound=$bound already=$already failed=$failed"
[ "$failed" -eq 0 ] || exit 1
