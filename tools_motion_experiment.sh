#!/bin/bash
# Which part of the motion sensing triggers the 2.8in's interrupt-watchdog restarts? Runs each debug mode up to 15 min.
IP=192.168.1.189; LOG=/home/grey/Documents/Feenove-Aquarium/motion_experiment.log
lines() { curl -s -m 6 http://$IP/api/resetlog | grep -c .; }
for mode in 0 2 1 3 4; do
  curl -s -m 8 -X POST "http://$IP/api/set?motion=0" >/dev/null; sleep 2
  curl -s -m 8 "http://$IP/api/mdbg?mode=$mode" >/dev/null
  curl -s -m 8 -X POST "http://$IP/api/set?motion=1" >/dev/null
  base=$(lines); t0=$(date +%s); result="SURVIVED 15 min"
  while [ $(( $(date +%s) - t0 )) -lt 900 ]; do
    sleep 10
    n=$(lines); [ -z "$n" ] && n=$base
    if [ "$n" -gt "$base" ]; then result="CRASHED after $(( $(date +%s) - t0 )) s"; break; fi
  done
  echo "$(date '+%F %T') mode $mode: $result" >> $LOG
done
curl -s -m 8 -X POST "http://$IP/api/set?motion=0" >/dev/null; curl -s -m 8 "http://$IP/api/mdbg?mode=0" >/dev/null
echo "$(date '+%F %T') done (motion left OFF, mode 0)" >> $LOG
