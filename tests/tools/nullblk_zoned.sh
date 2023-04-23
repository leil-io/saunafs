#!/bin/bash

rewrite_file(){
        echo "${1}" | sudo tee "${2}" >/dev/null
}

function create_zoned_nullb() {
        sudo modprobe null_blk nr_devices=0 || return $?

        local bs=$1
        local zs=$2
        local nr_conv=$3
        local nr_seq=$4
        local nid=$5

        cap=$(( zs * (nr_conv + nr_seq) ))

        dev="/sys/kernel/config/nullb/sauna_nullb${nid}"
        sudo mkdir "${dev}" 2>/dev/null || true

        rewrite_file "${bs}" "${dev}/blocksize"
        rewrite_file 0 "${dev}/completion_nsec"
        rewrite_file 0 "${dev}/irqmode"
        rewrite_file 2 "${dev}/queue_mode"
        rewrite_file 1024 "${dev}/hw_queue_depth"
        rewrite_file 1 "${dev}/memory_backed"
        rewrite_file 1 "${dev}/zoned"

        rewrite_file ${cap} "${dev}/size"
        rewrite_file ${zs} "${dev}/zone_size"
        rewrite_file ${nr_conv} "${dev}/zone_nr_conv"

        rewrite_file 1 "${dev}/power"

        if [ -d "/sys/block/sauna_nullb${nid}" ]; then
                rewrite_file mq-deadline "/sys/block/sauna_nullb${nid}/queue/scheduler"
        else
                rewrite_file mq-deadline "/sys/block/nullb${nid}/queue/scheduler"
        fi
}

function remove_all_emulated_zoned_disks() {
        for mount_point in $(mount | grep "sauna_nullb" | cut -d " " -f 3); do
                sudo umount -l "${mount_point}" &>/dev/null
        done

        for disk in $(ls -d /sys/kernel/config/nullb/sauna_nullb* 2>/dev/null || true); do
                rewrite_file 0 "${disk}/power"
                sudo rmdir "${disk}"
        done
}
