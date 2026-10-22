#!/bin/bash
# Which part of the motion sensing triggers the 2.8in's interrupt-watchdog restarts? Each mode: reboot, then motion on with that
# debug mode, watch the board's persistent crash counter for up to 12 minutes.
IP=192.168.1.189; LOG=/home/grey/Documents/Feenove-Aquarium/motion_experiment.log
count() { curl -s -m 6 http://$IP/api/status | grep -o '"crashes":[0-9]*' | cut -d: -f2; }
up() { curl -s -m 6 http://$IP/api/status | grep -o '"up":[0-9]*' | cut -d: -f2; }
for mode in 0 2 1 3 4; do
  curl -s -m 8 -X POST "http://$IP/api/set?motion=0" >/dev/null; sleep 2
  curl -s -m 8 "http://$IP/api/reboot" >/dev/null; sleep 40
  base=$(count); [ -z "$base" ] && { echo "$(date '+%F %T') mode $mode: board not answering after reboot" >> $LOG; continue; }
  curl -s -m 8 "http://$IP/api/mdbg?mode=$mode" >/dev/null
  curl -s -m 8 -X POST "http://$IP/api/set?motion=1" >/dev/null
  t0=$(date +%s); result="SURVIVED 12 min (no new crash)"
  while [ $(( $(date +%s) - t0 )) -lt 720 ]; do
    sleep 8
    n=$(count); [ -z "$n" ] && continue
    if [ "$n" -gt "$base" ]; then result="CRASHED after $(( $(date +%s) - t0 )) s ($((n-base)) new)"; break; fi
  done
  echo "$(date '+%F %T') mode $mode: $result" >> $LOG
done
curl -s -m 8 -X POST "http://$IP/api/set?motion=0" >/dev/null
echo "$(date '+%F %T') done (motion left OFF)" >> $LOG
