#!/bin/bash
# Polls the fish tank boards and appends their status + restart log to restart_watch.log.
LOG=/home/grey/Documents/Feenove-Aquarium/restart_watch.log
for h in fishtank fishtank28 cydfishtank; do
  ip=$(getent hosts $h.local | awk '{print $1}' | head -1)
  if [ -z "$ip" ]; then echo "$(date '+%F %T') $h: not found on the network" >> $LOG; continue; fi
  st=$(curl -s -m 8 http://$ip/api/status)
  if [ -z "$st" ]; then echo "$(date '+%F %T') $h ($ip): no answer" >> $LOG; continue; fi
  up=$(echo "$st" | grep -o '"up":[0-9]*' | cut -d: -f2); rst=$(echo "$st" | grep -o '"rst":"[^"]*"' | cut -d'"' -f4); ver=$(echo "$st" | grep -o '"ver":"[^"]*"' | cut -d'"' -f4)
  echo "$(date '+%F %T') $h ($ip) v$ver up=${up}s last-restart=$rst" >> $LOG
  rl=$(curl -s -m 8 http://$ip/api/resetlog)
  [ -n "$rl" ] && echo "$rl" | sed "s/^/    RESTART-LOG[$h]: /" >> $LOG
done

# PhotoFrame on the 4in Freenove (different firmware: its status uses "up" and "reset")
ip=$(getent hosts photoframe4.local | awk '{print $1}' | head -1)
if [ -n "$ip" ]; then
  st=$(curl -s -m 8 http://$ip/api/status)
  echo "$(date '+%F %T') photoframe4 ($ip) v$(echo "$st" | grep -o '"v":"[^"]*"' | cut -d'"' -f4) up=$(echo "$st" | grep -o '"up":[0-9]*' | cut -d: -f2)s reset=$(echo "$st" | grep -o '"reset":"[^"]*"' | cut -d'"' -f4)" >> $LOG
fi
