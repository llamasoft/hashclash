#!/usr/bin/env bash
# shellcheck disable=SC2064,SC2155,SC2012

export BINDIR=$(dirname "$0")/../bin
export BIRTHDAYSEARCH=$BINDIR/md5_birthdaysearch
export HELPER=$BINDIR/md5_diffpathhelper
export FORWARD=$BINDIR/md5_diffpathforward
export BACKWARD=$BINDIR/md5_diffpathbackward
export CONNECT=$BINDIR/md5_diffpathconnect
export CPUS=$(grep -c "^processor" /proc/cpuinfo || echo 0)
if [[ -n "$MAXCPUS" ]]; then
	if (( !CPUS || CPUS > MAXCPUS )); then
		export CPUS=$MAXCPUS
	fi
fi
export TTT=12
export EXPECTED_BLOCKTIME=$((3600 * 8 / CPUS))
export AUTO_KILL_TIME=$((EXPECTED_BLOCKTIME * 2))

rm -r data 2>/dev/null
mkdir data || exit 1

file1=$1
file2=$2
starttime=$(date +%s)

function notify {
	msg=$1
	echo "[*] $msg"
	if command -v notify-send &>/dev/null; then
		notify-send "HashClash" "$msg"
	fi
}

if [[ -f "$file1" && -f "$file2" ]]; then
	echo "Chosen-prefix file 1: $file1"
	echo "Chosen-prefix file 2: $file2"
else
	echo "Usage $0 <file1> <file2> [<redonearcollstep>] [<nobirthday>]"
	exit 1
fi

if [[ -z "$3" ]]; then
	# check for CUDA / AVX256 threads
	echo -n "Detecting worklevel..."
	worklevel=$($BIRTHDAYSEARCH --inputfile1 "$file1" --inputfile2 "$file2" --hybridbits 0 --pathtyperange 2 --maxblocks 9 --maxmemory 100 --threads "$CPUS" --cuda_enable |& grep "^Work" -m1 | head -n1 | cut -d'(' -f2 | cut -d'.' -f1)
	echo ": $worklevel"
	if (( worklevel >= 31 )); then
		$BIRTHDAYSEARCH --inputfile1 "$file1" --inputfile2 "$file2" --hybridbits 0 --pathtyperange 2 --maxblocks 7 --maxmemory 4000 --threads "$CPUS" --cuda_enable
	else
		$BIRTHDAYSEARCH --inputfile1 "$file1" --inputfile2 "$file2" --hybridbits 0 --pathtyperange 2 --maxblocks 9 --maxmemory 100 --threads "$CPUS" --cuda_enable
	fi
	notify "Birthday search completed."

	PREFIX1=file1.bin
	PREFIX2=file2.bin
	COLL1=$(ls "data/birthdayblock1_"*".bin" | head -n1)
	if [[ ! -f "$COLL1" ]]; then
		echo "Birthday block file $COLL1 not found"
		exit 1
	fi
	COLL2="${COLL1//birthdayblock1/birthdayblock2}"

	cat "$PREFIX1" "$COLL1" > file1_0.bin
	cat "$PREFIX2" "$COLL2" > file2_0.bin
else
	if [[ -n "$4" ]]; then
		cp "$file1" file1_0.bin
		cp "$file2" file2_0.bin
	fi
fi

function doforward {
	$FORWARD -w "$1" -f "$1"/lowerpath.bin.gz --normalt01 -t 1 --trange $((TTT-3)) --threads "$CPUS" || return 1
	$FORWARD -w "$1" -a 500000 -t $((TTT-1)) --trange 0 --threads "$CPUS" || return 1
}

function dobackward {
	$BACKWARD -w "$1" -f "$1"/upperpath.bin.gz -t 36 --trange 6 -a 65536 -q 128 --threads "$CPUS" || return 1
	$BACKWARD -w "$1" -t 29 -a 100000 --trange 8 --threads "$CPUS" || return 1
	$BACKWARD -w "$1" -t 20 -a 16384 --threads "$CPUS" || return 1
	$BACKWARD -w "$1" -t 19 -a 500000 --trange $((18-TTT-3)) --threads "$CPUS" || return 1
}

function testcoll {
	for f in "$1/coll1_"*; do
		if [[ -e "${f//coll1/coll2}" ]]; then return 0; fi
	done
	return 1
}

function doconnect {
	mkdir "$1"/connect
	$CONNECT -w "$1"/connect -t $TTT --inputfilelow "$1"/paths$((TTT-1))_0of1.bin.gz --inputfilehigh "$1"/paths$((TTT+4))_0of1.bin.gz --threads "$CPUS" &
	local CPID="$!"

	contime=0
	while true; do
		sleep 5
		ps -p $CPID &>/dev/null || break
		(( contime = contime + 5*CPUS ))
		if (( contime > 10000 )); then
			kill $CPID &>/dev/null
			break
		fi
	done
}

function docollfind {
	$HELPER -w "$1" --findcoll "$2"/bestpaths.bin.gz --threads "$CPUS" |& tee "$2"/collfind.log &
	local CPID=$!
	while true; do
		if testcoll "$1" ; then break; fi
		sleep 1
	done
	sleep 3
	kill $CPID &>/dev/null
}

function dostepk {
	local k=$1
	local pid=$$
	echo "[*] Starting step $k"
	sleep 1

	local workdir="workdir${k}"
	rm -r "$workdir" 2>/dev/null
	mkdir "$workdir"

	echo $$ > "$workdir/pid"
	set -o pipefail
	if ! $HELPER -w "$workdir" --startnearcollision "file1_${k}.bin" "file2_${k}.bin" --pathtyperange 2 |& tee "$workdir/start.log"; then
	    touch "$workdir/killed"
	    exit
	fi

	cp ./*.cfg "$workdir/"

	doforward "$workdir" |& tee "$workdir/forward.log"
	dobackward "$workdir" |& tee "$workdir/backward.log"

	doconnect "$workdir" |& tee "$workdir/connect.log"

	docollfind "$workdir" "$workdir/connect"

	for f1 in "$workdir/coll1_"*; do
		f2="${f1//coll1/coll2}"
		if [[ -e "$f2" ]]; then
			cat "file1_${k}.bin" "$f1" > "file1_$((k+1)).bin"
			cat "file2_${k}.bin" "$f2" > "file2_$((k+1)).bin"
			cp "file1_$((k+1)).bin" "${file1}.coll"
			cp "file2_$((k+1)).bin" "${file2}.coll"
			break;
		fi
	done
}

function auto_kill
{
	local workdir="$1"
	local pidfile="$workdir/pid"
	local killfile="$workdir/killed"
	local remaining=$2

	while (( remaining > 0 )); do
		echo "[*] Time before backtrack: $remaining s"
		sleep 10
		(( remaining -= 10 ))
	done

	local pid=$(<"$pidfile")
	echo "[*] Timeout reached. Killing process with pid $pid"
	touch "$killfile"
	pkill -KILL -P "$pid" &>/dev/null
	kill -KILL "$pid" &>/dev/null
}

#cp file1.bin file1_0.bin
#cp file2.bin file2_0.bin
k=0
if [[ -n "$3" ]]; then
	k=$3
fi

cat <<EOF >md5diffpathbackward.cfg.template
autobalance = 1000000
estimate = 4
fillfraction = 1
maxsdrs = 1
condtend = 35
EOF

cat <<EOF >md5diffpathforward.cfg.template
autobalance = 2000000
estimate = 4
fillfraction = 1
maxsdrs = 1
minQ456tunnel = 18
minQ91011tunnel = 18
EOF

cat <<EOF >md5diffpathconnect.cfg.template
Qcondstart = 21
EOF

workdir="workdir${k}"
backtracks=0
while true; do

	echo "[*] Number of backtracks until now: $backtracks"
	if (( backtracks > 20 )); then
		notify "More than 20 backtracks is not normal. Please restart from scratch."
		break
	fi


	# Check if the collision has been generated
	if [[ -f "${file1}.coll" && -f "${file2}.coll" ]]; then
		if [[ $(cat "${file1}.coll" | md5sum | cut -d' ' -f1) == $(cat "${file2}.coll" | md5sum | cut -d' ' -f1) ]]; then
			notify "Collision generated: ${file1}.coll ${file2}.coll"
			md5sum "${file1}.coll" "${file2}.coll"
			break
		fi
	fi

	# Start the autokiller
	auto_kill "$workdir" $AUTO_KILL_TIME &
	autokillerpid=$!
	mainpid=$$
	trap "kill $autokillerpid &>/dev/null; pkill -KILL -P $mainpid &>/dev/null; killall -r md5_ &>/dev/null; exit" TERM INT

	# Start the computation
	rm -f "step$k.log"
	(dostepk "$k" 2>&1 &) | tee "step$k.log"
	kill $autokillerpid &>/dev/null
	killall -r md5_ &>/dev/null

	# Check if the termination was completed or killed
	if [[ -f "$workdir/killed" ]]; then
		failedk=$k
		(( k = (k > 0 ? k-1 : 0) ))
		notify "Step $failedk failed. Backtracking to step $k"
		(( backtracks += 1 ))
	else
		notify "Step $k completed"
		(( k += 1 ))
	fi
	sleep 2
done

runtime=$((($(date +%s)-starttime)/60))
notify "Process completed in $runtime minutes ($backtracks backtracks)."

# kill any pending thing
rm md5diffpath*.cfg md5diffpath*.template
pkill -P $$ &>/dev/null
killall -r md5_ &>/dev/null
exit
