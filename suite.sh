#!/bin/bash

TOTAL_FARNODE_USAGE=(64 128 256)

NUM_HOGS=(4 8 16 32)

MEMHOGS=()

HOGS=()

MEM_PER_HOG=()

USAGE=()

MAP_MS=()

MERGE_MS=()

function unhog {
    if (( $(echo "${#MEMHOGS[@]} == 0" | bc -l) )); then return; fi
    >&2 echo ""
    >&2 echo "STDERR: Clearing memhogs"
    kill -9 "${MEMHOGS[@]}"
    MEMHOGS=()
    sleep 3
    >&2 echo "===="
}

function cleanup {
    unhog

    trap - EXIT
    trap - ERR
}

function run_tests {    
    >&2 echo "STDERR: Starting tests"
    >&2 echo "===="
    
    for hogs in "${NUM_HOGS[@]}"; do
        for usage in "${TOTAL_FARNODE_USAGE[@]}"; do
            local mem_per_hog=$(echo "scale=0; $usage / $hogs" |bc -l )

            >&2 echo "STDERR: Starting $hogs memhogs, each $mem_per_hog GB, total $usage GB"
            >&2 echo "----"

            # spin up the memhogs, each using alloted slice of memory
            for ((i=0; i < $hogs; i++)); do
                numactl -m 2,3 python3 memhog.py $mem_per_hog &
                MEMHOGS+=($!)
            done
            
            while (( 1 )); do
                >&2 printf "STDERR: Fault-in for 60 seconds."
                for ((i=0; i < 20; i++)); do
                    sleep 3
                    >&2 printf "."
                done
                >&2 printf "\n"
                >&2 echo "----"

                for hog in "${MEMHOGS[@]}"; do
                    local use=$(numastat -p "$hog" | grep "Total " | awk '{print $(NF)}')
                    >&2 echo "memhog $hog usage: $use mb"
                done

                # convert the mb usage to gb and compare against the intended amount
                local gb=$(echo "$use / 1000" | bc -l)

                if (( $(echo "$gb > $mem_per_hog" | bc -l) )); then break; fi

                >&2 echo "STDERR: Fault-in not complete, looping..."
            done
            >&2 echo "----"

            # run the test. grab only the map report
            local out=$(./mvmap2 2 3)

            # debug print
            # >&2 printf "$out"

            local map=$(printf "$out" | grep -A 10 "reverse mapping")

            # mush into a csv format
            local map_ms=$(printf "$map" | grep -e "\[main\] \[.*\]" | grep -o -e "[[:digit:]]*")
            local merge_ms=$(printf "$map" | grep -e "\[reverse_map\] \[.*\]" | grep -o -e "[[:digit:]]*")

            HOGS+=($hogs)
            MEM_PER_HOG+=($mem_per_hog)
            USAGE+=($usage)
            MAP_MS+=($map_ms)
            MERGE_MS+=($merge_ms)

            >&2 echo "STDERR: map time $map_ms ms, merge time $merge_ms ms"
            >&2 echo "===="

            unhog
        done
    done

    echo "number_of_hogs,mem_per_hog_gb,total_gb,map_ms,merge_ms"
    for ((i=0; i < "${#HOGS[@]}"; i++)); do
        echo "${HOGS[$i]},${MEM_PER_HOG[$i]},${USAGE[$i]},${MAP_MS[$i]},${MERGE_MS[$i]}"
    done
}

trap cleanup EXIT ERR

run_tests