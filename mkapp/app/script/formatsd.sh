#!/bin/sh

source /mnt/app/app/record/record-env.sh
/mnt/app/app/record/gogglecmd -rec quit
/mnt/app/app/record/gogglecmd -sds quit

if [ -e /mnt/extsd/resource ]; then
    cp -r /mnt/extsd/resource /tmp/
fi
sleep 2

echo "Umounting SD Card"
umount /mnt/extsd
if [ $? -eq 0 ]; then
    echo "Umounting SD Card: SUCCESS"
else
    echo "Umounting SD Card: FAILURE"
fi

DEV=/dev/mmcblk0
PART=/dev/mmcblk0p1

# Emit a number as <width> little-endian bytes (printf octal escapes).
emit_le() {
    n=$1
    w=$2
    i=0
    while [ $i -lt $w ]; do
        printf "\\$(printf '%03o' $((n & 255)))"
        n=$((n >> 8))
        i=$((i + 1))
    done
}

# Write a fresh MBR with one FAT32-LBA (type 0x0c) partition, 1 MiB aligned,
# spanning the card. This forces the partition type byte to match the FAT32
# filesystem written below. Without it, a leftover exFAT/NTFS (0x07) type byte
# from a prior format makes the card unreadable on macOS, which honors the type
# byte, even though the goggle's Linux vfat driver ignores it.
write_mbr() {
    sz=$(cat /sys/block/mmcblk0/size)
    start=2048
    count=$((sz - start))
    # Clear old table / FS signatures (first 1 MiB) and any GPT backup (last 1 MiB).
    dd if=/dev/zero of="$DEV" bs=512 count=2048 conv=notrunc 2>/dev/null
    # Partition entry @446: status, startCHS, type=0x0c, endCHS, startLBA, sectors.
    { printf '\000\376\377\377\014\376\377\377'; emit_le $start 4; emit_le $count 4; } \
        | dd of="$DEV" bs=1 seek=446 conv=notrunc 2>/dev/null
    # Boot signature.
    printf '\125\252' | dd of="$DEV" bs=1 seek=510 conv=notrunc 2>/dev/null
    dd if=/dev/zero of="$DEV" bs=512 seek=$((sz - 2048)) count=2048 conv=notrunc 2>/dev/null
    sync
}

rm -f /tmp/mkfs.result
echo "Writing partition table" > /tmp/mkfs.log
if [ ! -b "$DEV" ]; then
    echo "Device $DEV not found" >> /tmp/mkfs.log
    RESULT=1
else
    write_mbr >> /tmp/mkfs.log 2>&1
    partprobe "$DEV" >> /tmp/mkfs.log 2>&1
    # Wait for the re-read partition node to appear.
    i=0
    while [ ! -b "$PART" ] && [ $i -lt 10 ]; do
        sleep 1
        i=$((i + 1))
    done
    echo "Formatting $PART as FAT32" >> /tmp/mkfs.log
    mkfs.vfat -F 32 "$PART" -n "HDZERO" >> /tmp/mkfs.log 2>&1
    RESULT=$?
fi
echo "mkfs result: $RESULT" >> /tmp/mkfs.log
echo $RESULT > /tmp/mkfs.result

echo "Mounting SD Card"
mount "$PART" /mnt/extsd
if [ $? -eq 0 ]; then
    echo "Mounting SD Card: SUCCESS"
else
    echo "Mounting SD Card: FAILURE"
fi
sleep 1

if [ -e /tmp/resource ]; then
    cp -r /tmp/resource /mnt/extsd/
    rm -rf /tmp/resource
fi

/mnt/app/app/record/record &
/mnt/app/app/record/sdstat &

