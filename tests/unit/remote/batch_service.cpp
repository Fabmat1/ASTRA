// Unit tests for the pure parts of remote/RemoteBatchService: how fits are
// packed into tasks, what happens to a fit that came back without a result,
// how durations are estimated, and how the one-command poll of a batch and
// Slurm's squeue/sacct output are read. These decide whether a campaign
// keeps within a cluster's limits and whether a killed fit is retried where
// it can succeed, so they are tested away from any host.

#include <doctest.h>

#include "remote/RemoteBatchService.h"

#include <numeric>

using namespace astra::remote;

namespace {

int totalFits(const QVector<QVector<int>>& tasks)
{
    int n = 0;
    for (const auto& t : tasks) n += t.size();
    return n;
}

} // namespace

TEST_SUITE("remote")
{

TEST_CASE("packTasks: fits per task bounds each task")
{
    const QVector<double> est(45, 10.0);
    const auto tasks = packTasks(est, 20, 0, 0.0, 0);
    CHECK(tasks.size() == 3);
    CHECK(totalFits(tasks) == 45);
    for (const auto& t : tasks) CHECK(t.size() <= 20);
}

TEST_CASE("packTasks: a small batch spreads over the concurrency")
{
    // Ten fits on a host that runs 500 tasks at once: one each, not one task.
    const QVector<double> est(10, 60.0);
    const auto tasks = packTasks(est, 20, 500, 0.0, 0);
    CHECK(tasks.size() == 10);
    for (const auto& t : tasks) CHECK(t.size() == 1);
}

TEST_CASE("packTasks: task time bounds the packing, longest fits first")
{
    const QVector<double> est = { 100, 900, 300, 300, 50 };
    const auto tasks = packTasks(est, 10, 0, 1000.0, 0);
    CHECK(totalFits(tasks) == 5);
    for (const auto& t : tasks) {
        double load = 0.0;
        for (int i : t) load += est[i];
        CHECK(load <= 1000.0);
        for (int k = 1; k < t.size(); ++k) CHECK(est[t[k - 1]] >= est[t[k]]);
    }
}

TEST_CASE("packTasks: a fit longer than a whole task still gets one")
{
    const QVector<double> est = { 5000, 10 };
    const auto tasks = packTasks(est, 10, 0, 1000.0, 0);
    CHECK(totalFits(tasks) == 2);
    bool alone = false;
    for (const auto& t : tasks)
        if (t.size() == 1 && t[0] == 0) alone = true;
    CHECK(alone);
}

TEST_CASE("packTasks: the task cap leaves the rest for later")
{
    const QVector<double> est(100, 10.0);
    const auto tasks = packTasks(est, 5, 0, 0.0, 4);
    CHECK(tasks.size() == 4);
    CHECK(totalFits(tasks) == 20);        // 4 tasks x 5 fits; 80 wait
    CHECK(packTasks({}, 5, 0, 0.0, 0).isEmpty());
}

TEST_CASE("classifyExit: kills and crashes escalate, other exits retry")
{
    CHECK(classifyExit(137).fate == ItemFate::Escalate);
    CHECK(classifyExit(137).reason.contains("memory"));
    CHECK(classifyExit(143).fate == ItemFate::Escalate);
    CHECK(classifyExit(143).reason.contains("time"));
    CHECK(classifyExit(134).fate == ItemFate::Escalate);
    CHECK(classifyExit(134).reason.contains("crashed"));
    CHECK(classifyExit(139).fate == ItemFate::Escalate);
    CHECK(classifyExit(1).fate == ItemFate::Retry);

    const auto v = classifyExit(134, { "terminate called after throwing an instance "
                                       "of 'std::bad_alloc'", "  what():  std::bad_alloc" });
    CHECK(v.reason.contains("exit 134"));
    CHECK(v.reason.contains("std::bad_alloc' |   what()"));
}

TEST_CASE("classifyTaskEnd: never started requeues, kills escalate")
{
    CHECK(classifyTaskEnd("OUT_OF_MEMORY", false).fate == ItemFate::Requeue);
    CHECK(classifyTaskEnd("stopped", false).fate == ItemFate::Requeue);
    CHECK(classifyTaskEnd("OUT_OF_MEMORY", true).fate == ItemFate::Escalate);
    CHECK(classifyTaskEnd("TIMEOUT", true).fate == ItemFate::Escalate);
    CHECK(classifyTaskEnd("terminated", true).fate == ItemFate::Escalate);
    CHECK(classifyTaskEnd("NODE_FAIL", true).fate == ItemFate::Retry);
    CHECK(classifyTaskEnd("CANCELLED by 131038", true).fate == ItemFate::Retry);
    CHECK(classifyTaskEnd("", true).reason.contains("unknown"));
}

TEST_CASE("estimateFitSeconds: generous without history, learned with it")
{
    CHECK(estimateFitSeconds({}, 1) == doctest::Approx(180.0));
    CHECK(estimateFitSeconds({}, 10) == doctest::Approx(720.0));

    std::vector<RemoteFitUsage> u;
    for (int i = 0; i < 20; ++i) u.push_back({ 2, 0, 20.0, 1 << 20 });  // 10 s/spectrum
    const double e = estimateFitSeconds(u, 4);
    CHECK(e == doctest::Approx(1.25 * 10.0 * 4 + 15.0));
    CHECK(estimateFitSeconds(u, 0) >= 30.0);
}

TEST_CASE("parseBatchProbe: every section is read")
{
    const QString out =
        "ASTRA_OUTBOX\n"
        "0b8e4f2a-1111-2222-3333-444455556666.json\n"
        "ASTRA_EXIT\n"
        "aaaaaaaa-1111-2222-3333-444455556666 137\n"
        "ASTRA_STARTED\n"
        "0b8e4f2a-1111-2222-3333-444455556666\n"
        "aaaaaaaa-1111-2222-3333-444455556666\n"
        "ASTRA_TASKS\n"
        "0 done 1790000000\n"
        "3 stopped 1790000100\n"
        "ASTRA_POOL\n"
        "alive\n"
        "ASTRA_END\n";
    const BatchProbe p = parseBatchProbe(out);
    CHECK_FALSE(p.dirMissing);
    CHECK(p.outbox == QStringList{ "0b8e4f2a-1111-2222-3333-444455556666" });
    CHECK(p.exits.value("aaaaaaaa-1111-2222-3333-444455556666") == 137);
    CHECK(p.started.size() == 2);
    CHECK(p.taskStates.value(0) == "done");
    CHECK(p.taskStates.value(3) == "stopped");
    CHECK(p.poolAlive);
    CHECK_FALSE(p.poolFinished);

    CHECK(parseBatchProbe("ASTRA_GONE\n").dirMissing);
}

TEST_CASE("parseBatchProbe: the stderr tail of fits that left no result")
{
    const QString a = "aaaaaaaa-1111-2222-3333-444455556666";
    const QString b = "bbbbbbbb-1111-2222-3333-444455556666";
    const QString out =
        "ASTRA_EXIT\n" + a + " 134\n" + b + " 1\n"
        "ASTRA_EXITLOG\n"
        "@" + a + "\n"
        "| ing 4 spectra\n"
        "| [memory] budget 3072 MiB (set)\n"
        "| step 3\rstep 4\rstep 5\n"
        "| ASTRA_END\n"
        "| terminate called after throwing an instance of 'std::bad_alloc'\n"
        "@" + b + "\n"
        "ASTRA_STARTED\n" + a + "\n"
        "ASTRA_END\n";
    const BatchProbe p = parseBatchProbe(out);
    CHECK(p.exits.value(a) == 134);
    CHECK(p.exitLogs.value(a) == QStringList{
              "step 5", "ASTRA_END",
              "terminate called after throwing an instance of 'std::bad_alloc'" });
    CHECK(p.exitLogs.value(b).isEmpty());   // no stderr left behind
    CHECK(p.started == QStringList{ a });   // a log line is not a marker
}

TEST_CASE("parseSqueueArray / parseSacctArray read array tasks")
{
    const auto q = parseSqueueArray("0 RUNNING\n7 PENDING\n");
    CHECK(q.size() == 2);
    CHECK(q.value(7) == "PENDING");

    const auto a = parseSacctArray("1862700_0|COMPLETED\n"
                                   "1862700_3|OUT_OF_MEMORY\n"
                                   "1862700_4|CANCELLED by 131038\n"
                                   "1862700_[5-9]|PENDING\n"
                                   "1862701|COMPLETED\n");
    CHECK(a.size() == 3);
    CHECK(a.value(3) == "OUT_OF_MEMORY");
    CHECK(a.value(4) == "CANCELLED");
}

TEST_CASE("buildBatchScript: one array with the tier's resources")
{
    RemoteHost h;
    h.name = "cluster";
    h.type = RemoteHost::Type::Slurm;
    h.slurm.partition = "all";
    h.slurm.extraSbatchLines = "#SBATCH --qos=normal";
    h.envSetup = "module load foo";
    h.batch.cpusPerTask = 2;
    h.batch.maxConcurrentTasks = 450;
    h.batch.timeGuardSeconds = 300;
    const ResourceTier t{ 30, "12G", "05:00:00" };

    const QString s = buildBatchScript(h, "/work/u/astra", "/work/u/astra/batches/b1",
                                       "0123456789abcdef", t, 40);
    CHECK(s.contains("#SBATCH --array=0-39%450"));
    CHECK(s.contains("#SBATCH --mem=12G"));
    CHECK(s.contains("#SBATCH --time=05:00:00"));
    CHECK(s.contains("#SBATCH --cpus-per-task=2"));
    CHECK(s.contains("#SBATCH --partition=all"));
    CHECK(s.contains("#SBATCH --qos=normal"));
    CHECK(s.contains("module load foo"));
    CHECK(s.contains("batch-runner.sh /work/u/astra/batches/b1 /work/u/astra/bundle "
                     "2 18000 300 9216M ${SLURM_ARRAY_TASK_ID}"));

    h.batch.maxConcurrentTasks = 0;
    CHECK(buildBatchScript(h, "/w", "/w/b", "x", t, 1).contains("#SBATCH --array=0-0\n"));
}

TEST_CASE("memBudgetFor: three quarters of the tier, in MiB")
{
    CHECK(memBudgetFor("4G") == "3072M");
    CHECK(memBudgetFor("1000M") == "750M");
    CHECK(memBudgetFor("junk") == "-");
}

} // TEST_SUITE
