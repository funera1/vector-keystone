#!/bin/sh

DURATION="1h"
SKIP_LOAD=0
NUM_THREADS=2
NUM_LOOP=10
BASE_INTERVAL=100
DIFF_INTERVAL=10
DELAY=0
CPU_AFFINITY=""
MANUAL_AFFINITY=0

declare -a PIDS=()

cleanup() {
    echo "Cleaning up processes..."
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            echo "Killing process $pid"
            kill "$pid" 2>/dev/null
        fi
    done
}

trap cleanup SIGINT SIGTERM EXIT

count_cpus() {
    local s="$1"
    local count=0
    local token start end
    IFS=','
    for token in $s; do
        if [[ "$token" =~ ^([0-9]+)-([0-9]+)$ ]]; then
            start=${BASH_REMATCH[1]}
            end=${BASH_REMATCH[2]}
            if [ "$end" -lt "$start" ]; then
                echo "Invalid CPU range: $token" >&2
                exit 1
            fi
            count=$((count + end - start + 1))
        elif [[ "$token" =~ ^[0-9]+$ ]]; then
            count=$((count + 1))
        else
            echo "Invalid CPU affinity token: $token" >&2
            exit 1
        fi
    done
    echo "$count"
}

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
        --loops|-l)
            if [[ -n "$2" ]]; then
                NUM_LOOP="$2"
                shift 2
            else
                echo "Error: --loops option requires an argument."
                exit 1
            fi
            ;;
        --interval|-i)
            if [[ -n "$2" ]]; then
                BASE_INTERVAL="$2"
                shift 2
            else
                echo "Error: --interval option requires an argument."
                exit 1
            fi
            ;;
        --distance|-d)
            if [[ -n "$2" ]]; then
                DIFF_INTERVAL="$2"
                shift 2
            else
                echo "Error: --distance option requires an argument."
                exit 1
            fi
            ;;
        --threads|-n)
            if [[ -n "$2" ]] && [[ "$2" =~ ^[1-4]$ ]]; then
                NUM_THREADS="$2"
                shift 2
            else
                echo "Error: --threads option requires a number between 1 and 4."
                exit 1
            fi
            ;;
        --cpus|-c|--affinity)
            if [[ -n "$2" ]]; then
                CPU_AFFINITY="$2"
                MANUAL_AFFINITY=1
                shift 2
            else
                echo "Error: --cpus|--affinity option requires an argument."
                exit 1
            fi
            ;;
        --delay)
            if [[ -n "$2" ]]; then
                DELAY="$2"
                shift 2
            else
                echo "Error: --delay option requires an argument."
                exit 1
            fi
            ;;
        --skip-load|-s)
            SKIP_LOAD=1
            shift 1
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--duration|-t DURATION] [--loops|-l NUM_LOOP] [--interval|-i INTERVAL]"
            echo "            [--distance|-d DISTANCE] [--threads|-n NUM_THREADS] [--delay SECONDS]"
            echo "            [--skip-load|-s]"
            echo ""
            echo "Options:"
            echo "  --duration, -t DURATION       Set test duration (default: 1h)"
            echo "  --loops, -l NUM_LOOP          Set number of loop cycles (default: 10)"
            echo "  --interval, -i INTERVAL       Set base interval in us (default: 100)"
            echo "  --distance, -d DISTANCE       Set distance between threads in us (default: 10)"
            echo "  --threads, -n NUM_THREADS     Number of latency test threads (1-4, default: 2)"
            echo "  --cpus, -c AFFINITY           Manual CPU affinity list or range (e.g. 0-3 or 0,1,2,3)."
            echo "                                 Must specify exactly as many CPUs as threads."
            echo "  --delay SECONDS               Delay before starting test in seconds (default: 0)"
            echo "  --skip-load, -s               Skip iperf3 and stress-ng, run only latency tests"
            exit 1
            ;;
    esac
done

if [ "$MANUAL_AFFINITY" -eq 1 ]; then
    cpus_count=$(count_cpus "$CPU_AFFINITY")
    if [ "$cpus_count" -ne "$NUM_THREADS" ]; then
        echo "Error: provided CPU affinity ($CPU_AFFINITY) lists $cpus_count CPUs but NUM_THREADS is $NUM_THREADS." >&2
        exit 1
    fi
else
    CPU_AFFINITY="0-$((NUM_THREADS - 1))"
fi

echo "Running cyclictest with following parameters:"
echo "  Number of thread(s): $NUM_THREADS | CPU(s) affinities: $CPU_AFFINITY"
echo "  Max duration: $DURATION | Max loop: $NUM_LOOP"
echo "  Base interval: $BASE_INTERVAL | Diff interval: $DIFF_INTERVAL"

if [ "$SKIP_LOAD" -eq 0 ]; then
    echo "Starting iperf3 server..."
    iperf3 -s > iperf3.log &
    PIDS+=($!)
    
    echo "iperf3 server started. Start the iperf client before continuing."
    read -p "Press enter to continue..."
    
    echo "Running stress-ng and cyclictest for a duration of $DURATION."
    stress-ng --all 1 -t $DURATION -x netlink-task,swap --log-file stress-ng.log > /dev/null &
    PIDS+=($!)
else
    echo "Skipping iperf3 and stress-ng (latency tests only mode)"
fi

echo "Starting cyclictest-enclave."
/usr/share/keystone/examples/cyclictest.ke -- -l $NUM_LOOP -a$CPU_AFFINITY -vm -i$BASE_INTERVAL -d$DIFF_INTERVAL -p99 -t $NUM_THREADS --duration=$DURATION --delay=$DELAY > cyclictest.log &
cyclictest_pid=$!


wait "$cyclictest_pid"
cleanup
