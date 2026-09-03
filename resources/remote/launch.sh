# astra-launch v1
# Starts a detached gael-worker for one staged job directory on a plain SSH
# host and prints its PID. Run as:
#   sh launch.sh <jobdir> <threads> <bundledir>
# The worker survives the ssh session (setsid + no controlling stdio), writes
# progress to progress.log, stdout to fit.log, its heartbeat to status.json
# and the final result to result.json inside the job directory.

jobdir=$1
threads=$2
bundle=$3

cd "$jobdir" || exit 1

GAEL_PROGRESS=1 setsid "$bundle/bin/gael-worker" \
    --fit fit_input.json \
    --global global_settings.json \
    --result-json result.json \
    --status-file status.json \
    --threads "$threads" \
    > fit.log 2> progress.log < /dev/null &

pid=$!
echo "$pid" > run.pid
echo "$pid"
