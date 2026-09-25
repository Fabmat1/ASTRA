# astra-batch-pool v1
# Runs every task of a staged ASTRA fit batch on a plain host, <parallel> at
# a time (0 = cores / threads), detached from the ssh session. Run as:
#   sh batch-pool.sh <batchdir> <ntasks> <parallel> <bundledir> <threads> <limit-s> <guard-s> <mem-budget>
# Prints the pool's pid, which is also its process group: ASTRA stops the
# whole batch with kill -TERM -<pid>.

batch=$1
ntasks=$2
parallel=$3
shift 3

cd "$batch" || exit 1
runner=$(dirname "$0")/batch-runner.sh

# 0 = as many runners as the machine has cores for, at <threads> each.
if [ "$parallel" -le 0 ]; then
    cores=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
    parallel=$((cores / ${2:-1}))
    [ "$parallel" -ge 1 ] || parallel=1
fi

setsid sh -c '
    batch=$1; ntasks=$2; parallel=$3; runner=$4; shift 4
    i=0
    while [ "$i" -lt "$ntasks" ]; do echo "$i"; i=$((i + 1)); done |
        xargs -n 1 -P "$parallel" sh "$runner" "$batch" "$@"
    date +%s > pool.finished
' astra-pool "$batch" "$ntasks" "$parallel" "$runner" "$@" \
    > pool.log 2>&1 < /dev/null &

echo $! > pool.pid
echo $!
