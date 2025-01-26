#!/usr/bin/env bash

set -euo pipefail

usage() {
    echo
    echo "Usage: $0 -m MSG_NUM -b BIT_DIFF [-m MSG_NUM -b BIT_DIFF ...] [-a AUTOBALANCE] [-s CONNECT_START] [-e CONNECT_END] [-p] [-h]"
    echo "  -m MSG_NUM              The message number (0..15) to apply a bit difference to"
    echo "  -b BIT_DIFF             The bit within MSG_NUM (1..32) to apply a difference to"
    echo "  -s CONNECT_RANGE_START  The step number (3..58) to start attempting path connections (default 14 or MSG_NUM+3)"
    echo "  -e CONNECT_RANGE_END    The step number (3..58) to stop attempting path connections (default 22)"
    echo "  -a AUTOBALANCE          The maximum number of differential paths to attempt to connect per step (default 500000)"
    echo "  -p                      Seed with all possible upper paths"
    echo "  -h                      Displays this help message"
}

opterr() {
    { echo "ERROR": "$@"; usage; } >&2
    exit 1
}

# Message difference parameters
# Will generate a pair of colliding inputs with a difference in bit B (1..32)
# of message word M (0..15).  Note that MD5 treats words as little-endian,
# so M0 B1 is actually the lowest bit of the *fourth* byte.
MDIFFS=()
BDIFFS=()

# Attempt to connect the backward and forward paths at every step in this range.
# The steps we choose to connect at are actually somewhat important.
# The connection steps t-2..t+3 are going to be dense with bit conditions.
# Textcoll only makes use of the Q9 tunnel which requires flexibility in Q10 and Q11.
# Textcoll only solves up to Q24 before checking the resulting MD5 output.
# To avoid cluttering Q9..Q11 as much as possible and to keep most of the conditions
# where textcoll can actually solve them, 14 <= step <= 22 is recommended.
CONNECT_RANGE_START=14
CONNECT_RANGE_END=22

# Keep the best N differential paths between steps.
# NOTE: the connect stage compares *all* combinations of forward and backward paths.
# The total computation for the forward/backward path stages is around 2 * N.
# The computation for the connect stage is proportional to N^2.
AUTOBALANCE=500000

# Seed the backwards paths with all valid bit conditions that generate a first block
# that can be used with the second block's den Boer-Bosselaers near-collision attack.
# To prevent duplicate paths, we only seed with 16 "positive" differences.
# This option isn't guaranteed to result in better final paths, but it will generate
# more variations of upper paths that have fewer bit conditions.
# If enabled, it is recommended to also increase the autobalance parameter by 2-3x.
ALL_UPPER_PATHS=0

while getopts "m:b:s:e:a:ph" opt; do
    case "${opt}" in
        "m") MDIFFS+=( $(( OPTARG )) ) ;;
        "b") BDIFFS+=( $(( OPTARG )) ) ;;
        "s") CONNECT_RANGE_START=$(( OPTARG )) ;;
        "e") CONNECT_RANGE_END=$(( OPTARG )) ;;
        "a") AUTOBALANCE=$(( OPTARG )) ;;
        "p") ALL_UPPER_PATHS=1 ;;
        "h") usage && exit ;;
        ":") opterr "Option -${OPTARG} requires a value" ;;
        "?") opterr "Invalid option -${OPTARG}" ;;
    esac
done

# It's not a collision if both inputs are identical.  That's just cheating. ;P
if (( ${#MDIFFS[@]} == 0 && ${#BDIFFS[@]} == 0 )); then
    opterr "At least one pair of -m and -b arguments are required"
fi

if (( ${#MDIFFS[@]} != ${#BDIFFS[@]} )); then
    opterr "Must have same number of -m and -b arguments"
fi

DIFF_ARGS=()
LEAST_MSG_NUM=9999
for (( i = 0; i < ${#MDIFFS[@]}; i++ )); do
    M=$(( MDIFFS[i] ))
    B=$(( BDIFFS[i] ))
    if (( M < 0 || M > 15 )); then
        opterr "Option -m must be between 0 and 15"
    fi
    if (( B < 1 || B > 32 )); then
        opterr "Option -b must be between 1 and 32"
    fi
    if (( M < LEAST_MSG_NUM )); then
        LEAST_MSG_NUM=$(( M ))
    fi
    DIFF_ARGS+=( "--diffm${M}" "${B}" )
done

# The earliest we can reasonably connect the forward and backward paths is at step M+3,
# where M is the earliest message word difference.  The forward path needs to
# have differential conditions for the backward path to "cancel out".  Connecting the
# paths at step t involves bridging steps t-2 to t+3.  If the forward path doesn't
# have a message difference *before* step t-2 then there's nothing to connect to.
if (( CONNECT_RANGE_START > 58 )); then
    opterr "Option -s must be between 3 and 58"
fi
if (( CONNECT_RANGE_START < LEAST_MSG_NUM + 3 )); then
    CONNECT_RANGE_START=$(( LEAST_MSG_NUM + 3 ))
    echo "Adjusting connect range start to ${CONNECT_RANGE_START}"
fi
if (( CONNECT_RANGE_START < 14 )); then
    # Connecing before step 14 usually results in additional bit conditions being
    # placed on Q9..Q11 which greatly reduces the effectiveness of the Q9 tunnel.
    echo "WARNING: connecting before step 14 is not recommended."
    echo "  See this script's comments for details."
fi

# Technically there's a similar limitation for CONNECT_RANGE_END and the last appearance
# of a message word difference.  However, in practice, we almost never want to connect
# after step 22.  This is because textcoll depends heavily on the Q9/m9 tunnel that
# results in bit differences starting at Q25.  The algorithm only solves up to Q24, then
# modifies the bits of Q9 to brute-force satisfy the bit conditions at Q25 and beyond.
# If we connect after step 22, the bulk of the bit conditions will fall in the brute-force
# region of Q25..Q64.  This makes solving up to Q24 much easier, but each extra bit condition
# in Q25..Q64 essentially doubles the required number of attempts.
if (( CONNECT_RANGE_END > 58 )); then
    opterr "Option -e must be between 3 and 58"
fi
if (( CONNECT_RANGE_END < CONNECT_RANGE_START )); then
    CONNECT_RANGE_END=$(( CONNECT_RANGE_START ))
    echo "Adjusting connect range end to ${CONNECT_RANGE_END}"
fi
if (( CONNECT_RANGE_END > 22 )); then
    echo "WARNING: connecting after step 22 is not recommended."
    echo "  See this script's comments for details."
fi


# The differential paths must start with an identical IHV.
cat <<EOF > lowerpath.txt
Q-3:    |........ ........ ........ ........|
Q-2:    |........ ........ ........ ........|
Q-1:    |........ ........ ........ ........|
Q0:     |........ ........ ........ ........|
EOF

# Convert the text representation to binary for use with the diffpath utilities.
md5_diffpathhelper --pathfromtext --inputfile1 ./lowerpath.txt --outputfile1 lowerpath.bin.gz

# The output IHV of the first block must be identical except for the highest bit of each word.
[[ -f "upperpaths.bin.gz" ]] && rm "upperpaths.bin.gz"
for q in +{+,-}{+,-}{+,-}; do
    {
        echo "Q61: |${q:0:1}....... ........ ........ ........|"
        echo "Q62: |${q:1:1}....... ........ ........ ........|"
        echo "Q63: |${q:2:1}....... ........ ........ ........|"
        echo "Q64: |${q:3:1}....... ........ ........ ........|"
    } > "temp.txt"
    md5_diffpathhelper --pathfromtext --inputfile1 "temp.txt" --outputfile1 "temp.bin.gz"
    md5_diffpathhelper -j "upperpaths.bin.gz" -j "temp.bin.gz" --outputfile1 "upperpaths.bin.gz"
    rm "temp.txt" "temp.bin.gz"
    (( !ALL_UPPER_PATHS )) && break
done


DIFFPATH_ARGS=(
    # Our collection of "--diffmX Y" arguments
    "${DIFF_ARGS[@]}"
    # Keep the best N differential paths between steps
    --autobalance "${AUTOBALANCE}"
    # Run (autobalance * estimate) steps to choose a good --maxconditions for each step.
    # This helps reduce memory usage and skip overly-complicated differential paths.
    --estimate 4
)

# md5_diffpathbackward actually processes trange + 1 steps.
# It solves step t using Qt+1, Qt-1, Qt-2, and Wt to produce Qt-3.
# This is why we start at step 63, not 64, and stop three (plus one) steps before the connecting step.
bwork="${PWD}/bwork"
[[ ! -d "${bwork}" ]] && mkdir -p "${bwork}"

# Generate steps 63..END with only a single output file.
md5_diffpathbackward "${DIFFPATH_ARGS[@]}" \
  -w "${bwork}" -f "upperpaths.bin.gz" \
  --tstep 63 --trange $(( 63 - (CONNECT_RANGE_END + 3 + 1) )) 2>&1 \
  | tee "${bwork}/step_63_to_${CONNECT_RANGE_END}.log"

# Generate steps END..START with one output file per step.
for (( tstep = CONNECT_RANGE_END; tstep > CONNECT_RANGE_START; tstep -= 1 )); do
    md5_diffpathbackward "${DIFFPATH_ARGS[@]}" \
      -w "${bwork}" --tstep $(( tstep + 3 )) 2>&1 \
      | tee "${bwork}/step_${tstep}.log"
done


# Just like before, md5_diffpathforward actually does trange + 1 steps.
# It solves step t using Qt-3, Qt-2, Qt-1, and Wt to produce Qt+1.
# Our input IHV have no special criteria, so we don't need special t=0 or t=1 steps (--normalt01).
fwork="${PWD}/fwork"
[[ ! -d "${fwork}" ]] && mkdir -p "${fwork}"

# Generate steps 0..START with only a single output file.
md5_diffpathforward "${DIFFPATH_ARGS[@]}" \
  -w "${fwork}" -f "lowerpath.bin.gz" \
  --tstep 0 --trange $(( CONNECT_RANGE_START - 1 )) --normalt01 2>&1 \
  | tee "${fwork}/step_0_to_${CONNECT_RANGE_START}.log"

# Generate steps START..END with one output file per step.
for (( tstep = CONNECT_RANGE_START; tstep < CONNECT_RANGE_END; tstep += 1 )); do
    md5_diffpathforward "${DIFFPATH_ARGS[@]}" \
      -w "${fwork}" --tstep "${tstep}" --normalt01 2>&1 \
      | tee "${fwork}/step_${tstep}.log"
done


# Connect the lower and upper paths for every step in START..END.
for (( tstep = CONNECT_RANGE_START; tstep <= CONNECT_RANGE_END; tstep += 1 )); do
    cwork="${PWD}/connect/step_${tstep}"
    [[ ! -d "${cwork}" ]] && mkdir -p "${cwork}"
    echo
    echo "==================== Connect @ Step ${tstep} ===================="
    md5_diffpathconnect "${DIFF_ARGS[@]}" \
      -w "${cwork}" --tstep "${tstep}" \
      --inputfilelow "${fwork}/paths$(( tstep - 1 ))_0of1.bin.gz" \
      --inputfilehigh "${bwork}/paths$(( tstep + 4 ))_0of1.bin.gz" \
      --Qcondstart $(( tstep + 4 )) 2>&1 \
      | tee "${cwork}.log"
done

echo
grep -F "Best path:" "${PWD}/connect/step_"*".log" | grep -vF "p=" | sort -t"=" -k4rn
