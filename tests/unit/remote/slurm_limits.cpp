// Unit tests for the Slurm limit detection behind the settings page's
// Detect button. The inputs are real command output captured from a
// university cluster (sacctmgr's -P format, with an association that sets no
// default QoS), plus the shapes other sites produce: explicit --qos in the
// user's sbatch lines, association-level caps, and clusters that report
// nothing at all.

#include <doctest.h>

#include "remote/RemoteScheduler.h"

using namespace astra::remote;

namespace {

const char* kConfig =
    "Configuration data as of 2026-09-25T01:40:00\n"
    "MaxArraySize            = 1001\n"
    "MaxJobCount             = 50000\n";

const char* kAssoc = "cluster|users||heavy,long,normal,short|||||\n";

const char* kQos =
    "normal|1000|500|cpu=1000,gres/gpu:tesla_v100=6,gres/gpu=20,mem=4T|5-00:00:00\n"
    "short|1000|30||01:00:00\n"
    "long|100|20|cpu=500,mem=1500G|30-00:00:00\n"
    "heavy|1000|500|cpu=1000,gres/gpu:tesla_v100=6,mem=4T|5-00:00:00\n"
    "no_submit||0||\n";

} // namespace

TEST_SUITE("remote")
{

TEST_CASE("SlurmLimits: no requested or default QoS falls back to normal")
{
    const SlurmLimits l = parseSlurmLimits(kConfig, kAssoc, kQos, {});
    CHECK(l.maxArraySize == 1001);
    CHECK(l.qos == "normal");
    CHECK(l.maxSubmit == 1000);
    CHECK(l.maxRunningJobs == 500);
    CHECK(l.maxCpus == 1000);
    CHECK(l.maxMemBytes == 4LL * 1024 * 1024 * 1024 * 1024);
    CHECK(l.maxWallSeconds == 5 * 86400);
    CHECK(l.notes.join(' ').contains("assuming"));
}

TEST_CASE("SlurmLimits: a --qos in the sbatch lines picks that QoS")
{
    const QString q = qosFromSbatchLines("#SBATCH --mem=4G\n#SBATCH --qos=long\n");
    CHECK(q == "long");
    CHECK(qosFromSbatchLines("#SBATCH --qos short") == "short");
    CHECK(qosFromSbatchLines("#SBATCH --time=1:00:00").isEmpty());

    const SlurmLimits l = parseSlurmLimits(kConfig, kAssoc, kQos, q);
    CHECK(l.qos == "long");
    CHECK(l.maxSubmit == 100);
    CHECK(l.maxRunningJobs == 20);
    CHECK(l.maxCpus == 500);
    CHECK(l.maxWallSeconds == 30 * 86400);
}

TEST_CASE("SlurmLimits: a QoS the account may not use is pointed out")
{
    const SlurmLimits l = parseSlurmLimits(kConfig, kAssoc, kQos, "no_submit");
    CHECK(l.notes.join(' ').contains("not among"));
}

TEST_CASE("SlurmLimits: association caps and the tighter limit win")
{
    const QString assoc =
        "cl|other|||||||\n"
        "cl|lab|gpu|normal,big|big|200|50|cpu=64,mem=256G|cpu=32\n";
    const QString qos = "big|500|100|cpu=128|2-00:00:00\n";
    const SlurmLimits l = parseSlurmLimits(kConfig, assoc, qos, {}, "lab");
    CHECK(l.qos == "big");                 // the association's default QoS
    CHECK(l.maxSubmit == 200);             // assoc 200 beats QoS 500
    CHECK(l.maxRunningJobs == 50);
    CHECK(l.maxCpus == 64);                // GrpTRES 64 beats QoS 128
    CHECK(l.maxMemBytes == 256LL * 1024 * 1024 * 1024);
    CHECK(l.maxWallSeconds == 2 * 86400);
}

TEST_CASE("SlurmLimits: a cluster that reports nothing leaves no limits")
{
    const SlurmLimits l = parseSlurmLimits("", "", "", {});
    CHECK(l.maxArraySize == 0);
    CHECK(l.maxSubmit == 0);
    CHECK(l.maxRunningJobs == 0);
    CHECK(l.maxCpus == 0);
    CHECK(l.maxMemBytes == -1);
    CHECK(l.maxWallSeconds == -1);
}

TEST_CASE("SlurmLimits: applying them sets the batch ceilings")
{
    BatchSettings b;
    b.cpusPerTask = 4;
    applySlurmLimits(parseSlurmLimits(kConfig, kAssoc, kQos, {}), b);
    CHECK(b.maxArraySize == 1001);
    CHECK(b.maxQueuedTasks == 1000);
    CHECK(b.maxConcurrentTasks == 250);    // 1000 CPUs / 4 per task < 500 jobs

    b.cpusPerTask = 1;
    applySlurmLimits(parseSlurmLimits(kConfig, kAssoc, kQos, {}), b);
    CHECK(b.maxConcurrentTasks == 500);    // now the running-job cap binds

    applySlurmLimits(SlurmLimits{}, b);
    CHECK(b.maxConcurrentTasks == 0);
    CHECK(b.maxQueuedTasks == 0);
}

} // TEST_SUITE
