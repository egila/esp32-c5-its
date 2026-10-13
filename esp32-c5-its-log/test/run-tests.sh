#!/bin/sh
# Convert synthetic logs in inspect mode and compare the record counts.
# usage: run-tests.sh [<log-cvt binary>]   (default ../src/log-cvt)

set -u
dir=$(cd "$(dirname "$0")" && pwd)
cvt=${1:-$dir/../src/log-cvt}
[ -x "$cvt" ] || { echo "log-cvt not found: $cvt" ; exit 1 ; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

python3 "$dir/mklogs.py" "$tmp" || exit 1

fail=0

# check <its/gps/info/version> <log> [option]
check()
{
  got=$("$cvt" ${3-} "$tmp/$2" 2>/dev/null |
        awk '/^Count:/ { c = 1 } c && /^(its|gps|info|version):/ { printf "%s%s", s, $2 ; s = "/" }')
  if [ "$got" = "$1" ] ; then
    echo "ok    $2${3:+ $3}: $got"
  else
    echo "FAIL  $2${3:+ $3}: $got, expected $1"
    fail=1
  fi
}

check 10/1/1/1 clean.usb -u
check 10/1/1/1 clean.sd
check 20/0/2/2 reset-after-magic.usb -u
check 10/0/1/1 overlap.usb -u
check 2/0/1/1 unknown-type.usb -u
check 21/0/1/1 cut-record.usb -u
check 3/0/2/2 cut-record-end.usb -u

# a fifo cannot be seeked: its records are read without look-ahead
mkfifo "$tmp/fifo.usb"
cat "$tmp/clean.usb" > "$tmp/fifo.usb" &
check 10/1/1/1 fifo.usb -u

# a usb log read as sd log stops at the first header
if "$cvt" "$tmp/clean.usb" 2>&1 >/dev/null | grep -q 'invalid record header at offset 0' ; then
  echo "ok    clean.usb: invalid record header at offset 0"
else
  echo "FAIL  clean.usb: no 'invalid record header at offset 0'"
  fail=1
fi

exit $fail
