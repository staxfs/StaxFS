#!/bin/bash

name=dfs-prototype

function kill_servers() {
  ROLE=$1
  SIGNAL=$2

  if [ -f /tmp/$name-$ROLE-pids ]; then
    echo "Killing old $ROLE servers"
    declare -i i=0
    for pid in $(cat /tmp/$name-$ROLE-pids)
    do
      echo "Killing $pid"
      # Wait for the process to die
      sudo kill -$SIGNAL $pid 2>/dev/null || true
      while sudo kill -0 $pid 2>/dev/null; do
        sleep 0.3
        sudo kill -$SIGNAL $pid 2>/dev/null || true
      done
    done
    rm /tmp/$name-$ROLE-pids
  fi
}

function start_servers() {
  ROLE=$1
  COUNT=$(($2 - 1))
  SERVER_BIN=$3
  CONF_DIR=$4
  
  kill_servers $ROLE 15
  for i in $(seq 0 $COUNT)
  do
    LOG_FILENAME=$name-$ROLE-$i.log
    STDERR_LOG_FILENAME=$name-$ROLE-$i.stderr.log
    PID=$(sudo bash -c "SPDLOG_LEVEL=$LOG_LEVEL \
    DFS_LOG_FILENAME=$LOG_FILENAME \
    DFS_LOG_FLUSH_INTERVAL=1 \
    nohup $SERVER_BIN \
    -f $CONF_DIR/$ROLE-$i.toml -r $ROLE -i $i \
    1>/dev/null 2>$STDERR_LOG_FILENAME &
    echo \$!")
    echo $PID >> /tmp/$name-$ROLE-pids
    echo "Started $ROLE server $i with pid $PID log file $LOG_FILENAME"
  done
}