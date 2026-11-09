#!/bin/bash
# usage: corebusy.sh SECONDS CPU... ; prints busy % per cpu over the interval
s=$1; shift
snap() { for c in "$@"; do awk -v c="cpu$c" '$1==c{t=0;for(i=2;i<=NF;i++)t+=$i; print c, t, $5+$6}' /proc/stat; done; }
a=$(snap "$@"); sleep "$s"; b=$(snap "$@")
paste <(echo "$a") <(echo "$b") | awk '{dt=$5-$2; di=$6-$3; printf "%s busy=%.1f%%\n",$1,100*(1-di/dt)}'
echo "loadavg: $(cat /proc/loadavg)"
