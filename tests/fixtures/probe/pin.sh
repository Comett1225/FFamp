#!/bin/bash
set -eu
cd "$(dirname "$0")/../.."
A=../build/amedia
PX=fixtures/probe
mkdir -p $PX/expected
: > $PX/checksums.txt
for f in $PX/*.mp4 $PX/*.mov $PX/*.m4a $PX/*.ts $PX/*.m2ts; do
    name=$(basename $f)
    python3 probe_check.py write $f $PX/expected/$name.json
    echo "$($A probe --json --packets $f | md5) packets $name" >> $PX/checksums.txt
    tracks=$($A probe $f | wc -l)
    for ((t = 0; t < tracks; t++)); do echo "$($A dump $f --track $t | md5) track$t $name" >> $PX/checksums.txt; done
    case $name in *.ts | *.m2ts) echo "$($A decode $f | md5) decode $name" >> $PX/checksums.txt ;; esac
done
wc -l $PX/checksums.txt
