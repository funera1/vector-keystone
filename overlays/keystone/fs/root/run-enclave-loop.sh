#!/bin/bash

pid=
cleanup() {
    if [ -n "$pid" ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

while true; do
    echo "running enclave"
    /usr/share/keystone/examples/loop-task.ke &
    pid=$!
    wait "$pid"
    pid=
    echo "waiting"
    sleep 1
done
