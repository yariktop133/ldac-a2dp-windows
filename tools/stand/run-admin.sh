#!/bin/sh
# Host side: copy a script to the test VM as C:\stand\admin.ps1, run it as SYSTEM
# through the StandAdmin task (stand-admin-setup.ps1) and print its output.
#   tools/stand/run-admin.sh script.ps1 [wait-seconds]
# VM credentials: btp/vm-pw.tmp (not in git).
set -e
export MSYS_NO_PATHCONV=1
VB="${VBOXMANAGE:-/c/Program Files/Oracle/VirtualBox/VBoxManage.exe}"
VM="${VM:-LdacTestVM}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd -W 2>/dev/null || pwd)"
GC="--username tester --passwordfile $ROOT/btp/vm-pw.tmp"
cp "$1" "${TMP:-/tmp}/admin.ps1"
"$VB" guestcontrol "$VM" copyto $GC --target-directory C:/stand/ "$(cygpath -w "${TMP:-/tmp}/admin.ps1" 2>/dev/null || echo "${TMP:-/tmp}/admin.ps1")"
# guestcontrol drops backslashes from arguments: forward slashes only
PS="--exe C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -- -NoProfile -Command"
"$VB" guestcontrol "$VM" run $GC $PS "Remove-Item C:/stand/admin.log -ErrorAction 0; schtasks /run /tn StandAdmin" >/dev/null
sleep "${2:-5}"
"$VB" guestcontrol "$VM" run $GC $PS "Get-Content C:/stand/admin.log"
