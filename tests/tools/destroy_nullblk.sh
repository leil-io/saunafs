#!/bin/bash

if [ $# != 1 ]; then
    echo "Usage: $0 <nullb ID>"
    exit 1
fi

nid=$1

if [ ! -b "/dev/sauna_nullb$nid" ]; then
    echo "/dev/sauna_nullb$nid: No such device"
    exit 1
fi

echo 0 > /sys/kernel/config/nullb/sauna_nullb$nid/power
rmdir /sys/kernel/config/nullb/sauna_nullb$nid

echo "Destroyed /dev/sauna_nullb$nid"
