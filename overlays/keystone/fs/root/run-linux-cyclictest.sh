#!/bin/sh

# Initialize variables
DURATION="1h"
SKIP_LOAD=0
RUN_ENCLAVE_LOOP=0

declare -a PIDS=()

cleanup() {
    echo "Cleaning up processes..."
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            echo "Killing process $pid"
            kill "$pid" 2>/dev/null
        fi
    done
    exit
}

trap cleanup SIGINT SIGTERM EXIT

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration|-t)
            if [[ -n "$2" ]]; then
                DURATION="$2"
                shift 2
            else
                echo "Error: --duration option requires an argument."
                exit 1
            fi
            ;;
        --skip-load|-s)
            SKIP_LOAD=1
            shift 1
            ;;
        --enclave-loop|-e)
            RUN_ENCLAVE_LOOP=1
            shift 1
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--duration|-t DURATION] [--skip-load|-s] [--enclave-loop|-e]"
            echo ""
            echo "Options:"
            echo "  --duration, -t DURATION       Set test duration (default: 1h)"
            echo "  --skip-load, -s               Skip iperf3 and stress-ng stressors"
            echo "  --enclave-loop, -e            Run enclave loop task"
            exit 1
            ;;
    esac
done


if [ "$SKIP_LOAD" -eq 0 ]; then
    echo "Starting iperf3 server..."

    iperf3 -s > iperf3.log &
    iperf3_pid=$!

    echo "iperf3 server started. Start the iperf client before continuing."

    read -p "Press enter to continue..."
fi

echo "Running stress-ng and cyclictest for a duration of $DURATION..."

if [ "$SKIP_LOAD" -eq 0 ]; then
    stress-ng --all 1 -t $DURATION -x netlink-task,swap --log-file stress-ng.log > /dev/null &

    PIDS+=($!)
fi

if [ "$RUN_ENCLAVE_LOOP" -eq 1 ]; then
    ./run-enclave-loop.sh &
fi

PIDS+=($!)

cyclictest -vm -i100 -p99 -t --duration=$DURATION > cyclictest.log &

cyclictest_pid=$!


wait "$cyclictest_pid"
cleanup

echo "Done"
