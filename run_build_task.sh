#!/bin/bash
cd /home/grey/Documents/Feenove-Aquarium
LOG=/home/grey/Documents/Feenove-Aquarium/build_task.log
echo "start $(date)" > $LOG
timeout 3h /home/grey/.local/bin/claude -p "$(cat BUILD_TASK.md)" --allowedTools "Bash Read Write Edit Glob Grep" >> $LOG 2>&1
echo "exit $? $(date)" >> $LOG
