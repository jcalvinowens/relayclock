#!/bin/bash -e

CALB="${1}"
if [ -z "${CALB}" ]; then
	CALB="0"
fi

EPOCH="$(date +%s)"
TGT="$[$EPOCH + 2]"

NOW="$(date -d @${TGT} "+%y %m %d %H %M %S")"
YR_T=${NOW:0:1}
YR_O=${NOW:1:1}
MO_T=${NOW:3:1}
MO_O=${NOW:4:1}
DY_T=${NOW:6:1}
DY_O=${NOW:7:1}
HR_T=${NOW:9:1}
HR_O=${NOW:10:1}
MN_T=${NOW:12:1}
MN_O=${NOW:13:1}
SC_T=${NOW:15:1}
SC_O=${NOW:16:1}

gdb-multiarch ./relayclock.elf -ex 'target extended-remote localhost:3333' <<< " \
set var CALB=${CALB}
set var YR_T=${YR_T}
set var YR_O=${YR_O}
set var MO_T=${MO_T}
set var MO_O=${MO_O}
set var DY_T=${DY_T}
set var DY_O=${DY_O}
set var HR_T=${HR_T}
set var HR_O=${HR_O}
set var MN_T=${MN_T}
set var MN_O=${MN_O}
set var SC_T=${SC_T}
set var SC_O=${SC_O}"

while :; do
	if [ "$(date +%s)" -eq "${TGT}" ]; then
		break
	fi
done

gdb-multiarch ./relayclock.elf -ex 'target extended-remote localhost:3333' <<< " \
set var ready_set=1"
