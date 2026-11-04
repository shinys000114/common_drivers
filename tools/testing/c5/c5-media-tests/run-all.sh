#!/bin/bash
# run-all.sh [--with-net] [--with-kms] [--with-stress]: run every test in order and print the summary of this run.
# --with-net adds the YouTube test, --with-kms the kmssink test that stops GDM, --with-stress the JPEG encoder
# start/stop and kill stress (82).
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo $0 $*"; exit 1; }
NET=0; KMS=0; STRESS=0
for a in "$@"; do case $a in --with-net) NET=1;; --with-kms) KMS=1;; --with-stress) STRESS=1;; esac; done
START=$(date +%Y%m%d-%H%M%S)
for t in [0-9][0-9]-*.sh; do
	case $t in 24-*) [ $NET = 1 ] || continue;; 70-*) [ $KMS = 1 ] || continue;; 82-*) [ $STRESS = 1 ] || continue;; esac
	echo "=== $t"
	if [ "${t%%-*}" = 70 ]; then bash "$t" --stop-gdm; else bash "$t"; fi
	sleep 5
done
echo; echo "=== summary since $START"
awk -F'\t' -v s="$START" 'NR==1 || $1 >= s' results/summary.tsv | column -t -s "$(printf '\t')"
