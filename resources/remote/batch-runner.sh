# astra-batch-runner v1
# Works through one task of a staged ASTRA fit batch. Run as:
#   sh batch-runner.sh <batchdir> <bundledir> <threads> <limit-s> <guard-s> <mem-budget> <task>
# <task> comes last so a pool can append it (xargs) and Slurm can pass
# $SLURM_ARRAY_TASK_ID. <limit-s> is the task's wall time (0 = none),
# <mem-budget> the GAEL cache ceiling ("-" = let GAEL decide).
#
# tasks/<task>.list holds "<item> <expected seconds>" per line. For every
# item the runner leaves exactly one of:
#   outbox/<item>.json   the fit's result (done, failed or aborted)
#   exit/<item>          the worker's exit status when it left no result
#                        (137 = killed, typically out of memory; 143 = the
#                        task itself was terminated, typically out of time)
# and marks it started/<item> before it begins. Items not started are left
# for ASTRA to batch again; cancel/<item> makes the runner skip one.
# tasks/<task>.state ends as done, stopped (time guard) or terminated. The
# guard never stops the task's first fit.

batch=$1
bundle=$2
threads=$3
limit=$4
guard=$5
mem=$6
task=$7

cd "$batch" || exit 1
mkdir -p outbox started exit cancel

state() {
    printf '%s %s\n' "$1" "$(date +%s)" > "tasks/$task.state.tmp" &&
        mv "tasks/$task.state.tmp" "tasks/$task.state"
}

term=0
child=0
on_term() {
    term=1
    [ "$child" -gt 0 ] && kill -TERM "$child" 2>/dev/null
}
trap on_term TERM INT

memarg=
[ "$mem" != "-" ] && memarg="--mem-budget $mem"

start=$(date +%s)
state running
stopped=0
first=1

while read -r item est; do
    [ -n "$item" ] || continue
    [ -f "outbox/$item.json" ] && continue
    [ -f "exit/$item" ] && continue
    [ -f "cancel/$item" ] && continue

    # The first fit always runs: an estimate longer than the whole task
    # would otherwise leave it waiting forever. ASTRA moves such fits to a
    # longer tier when they get killed.
    if [ "$limit" -gt 0 ] && [ "$first" -eq 0 ]; then
        left=$((limit - ($(date +%s) - start)))
        if [ "$left" -lt $((guard + ${est:-0})) ]; then
            stopped=1
            break
        fi
    fi

    first=0
    printf '%s %s\n' "$task" "$(date +%s)" > "started/$item"

    # In the background so the TERM trap runs at once, not after the fit.
    (
        cd "items/$item" || exit 1
        exec "$bundle/bin/gael-worker" --fit fit_input.json \
            --global global_settings.json --result-json result.json \
            --threads "$threads" --no-plots --no-pdf $memarg \
            > fit.log 2> progress.log < /dev/null
    ) &
    child=$!
    wait "$child"
    rc=$?
    # A trap interrupts wait; the worker is still writing its result then.
    while kill -0 "$child" 2>/dev/null; do
        wait "$child"
        rc=$?
    done
    child=0

    if [ "$term" -eq 1 ]; then
        echo 143 > "exit/$item"
        state terminated
        exit 143
    fi

    if [ -f "items/$item/result.json" ]; then
        mv "items/$item/result.json" "outbox/$item.json" && rm -rf "items/$item"
    else
        echo "$rc" > "exit/$item"
    fi
done < "tasks/$task.list"

if [ "$stopped" -eq 1 ]; then state stopped; else state done; fi
exit 0
