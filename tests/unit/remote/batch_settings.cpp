// Unit tests for remote/BatchSettings.
//
// Every site-specific number of batched remote fitting lives in these
// settings, so the parsing of the Slurm size and time syntaxes, the tier a
// fit lands in, and the escalation after a kill are what decide whether a
// campaign fits the user's cluster at all. A host saved before batching
// existed must also keep loading with sensible defaults.

#include <doctest.h>

#include "remote/BatchSettings.h"
#include "remote/RemoteHost.h"

#include <QJsonObject>

using namespace astra::remote;

TEST_SUITE("remote")
{

TEST_CASE("BatchSettings: memory sizes parse in Slurm's syntax")
{
    const long long MiB = 1024LL * 1024LL;
    CHECK(parseMemorySize("4G")    == 4096 * MiB);
    CHECK(parseMemorySize("512M")  == 512 * MiB);
    CHECK(parseMemorySize("1.5G")  == 1536 * MiB);
    CHECK(parseMemorySize("2gb")   == 2048 * MiB);
    CHECK(parseMemorySize("1T")    == 1024LL * 1024 * MiB);
    // A bare number is megabytes to Slurm, bytes when asked for.
    CHECK(parseMemorySize("4000")  == 4000 * MiB);
    CHECK(parseMemorySize("4000", false) == 4000);
    CHECK(parseMemorySize("")      == -1);
    CHECK(parseMemorySize("lots")  == -1);
    CHECK(parseMemorySize("-4G")   == -1);
}

TEST_CASE("BatchSettings: every Slurm time form parses")
{
    CHECK(parseSlurmTime("30")         == 30 * 60);           // minutes
    CHECK(parseSlurmTime("30:15")      == 30 * 60 + 15);      // min:sec
    CHECK(parseSlurmTime("05:00:00")   == 5 * 3600);
    CHECK(parseSlurmTime("1-00:00:00") == 86400);
    CHECK(parseSlurmTime("2-12")       == 2 * 86400 + 12 * 3600);
    CHECK(parseSlurmTime("1-02:30")    == 86400 + 2 * 3600 + 30 * 60);
    CHECK(parseSlurmTime("")           == -1);
    CHECK(parseSlurmTime("5h")         == -1);
    CHECK(parseSlurmTime("1:2:3:4")    == -1);
}

TEST_CASE("BatchSettings: a fit lands in the first tier that covers it")
{
    BatchSettings b;
    b.tiers = { { 10, "4G", "1:00:00" }, { 30, "8G", "2:00:00" },
                { 0, "32G", "10:00:00" } };
    CHECK(b.tierFor(1)   == 0);
    CHECK(b.tierFor(10)  == 0);
    CHECK(b.tierFor(11)  == 1);
    CHECK(b.tierFor(30)  == 1);
    CHECK(b.tierFor(31)  == 2);
    CHECK(b.tierFor(500) == 2);

    // Without an unbounded last tier, the biggest fits still go to the last.
    b.tiers.pop_back();
    CHECK(b.tierFor(500) == 1);

    b.tiers.clear();
    CHECK(b.tierFor(1) == -1);
}

TEST_CASE("BatchSettings: a kill escalates one tier, and not past the last")
{
    BatchSettings b;
    b.tiers = { { 10, "4G", "1:00:00" }, { 0, "8G", "2:00:00" } };
    CHECK(b.escalate(0)  == 1);
    CHECK(b.escalate(1)  == -1);
    CHECK(b.escalate(-1) == -1);
}

TEST_CASE("BatchSettings: a round trip is lossless")
{
    BatchSettings b;
    b.enabled            = false;
    b.fitsPerTask        = 7;
    b.cpusPerTask        = 2;
    b.maxArraySize       = 1001;
    b.maxConcurrentTasks = 450;
    b.maxQueuedTasks     = 900;
    b.timeGuardSeconds   = 120;
    b.maxRetries         = 3;
    b.learnFromUsage     = false;
    b.tiers = { { 5, "2G", "30" }, { 0, "64G", "15:00:00" } };

    const BatchSettings back = BatchSettings::fromJson(b.toJson());
    CHECK(back.enabled == b.enabled);
    CHECK(back.fitsPerTask == b.fitsPerTask);
    CHECK(back.cpusPerTask == b.cpusPerTask);
    CHECK(back.maxArraySize == b.maxArraySize);
    CHECK(back.maxConcurrentTasks == b.maxConcurrentTasks);
    CHECK(back.maxQueuedTasks == b.maxQueuedTasks);
    CHECK(back.timeGuardSeconds == b.timeGuardSeconds);
    CHECK(back.maxRetries == b.maxRetries);
    CHECK(back.learnFromUsage == b.learnFromUsage);
    CHECK(back.tiers == b.tiers);
}

TEST_CASE("BatchSettings: a host saved before batching gets the defaults")
{
    QJsonObject old;
    old["id"]   = "h1";
    old["name"] = "cluster";
    old["type"] = "slurm";
    const RemoteHost h = RemoteHost::fromJson(old);
    const BatchSettings def;
    CHECK(h.batch.enabled == def.enabled);
    CHECK(h.batch.fitsPerTask == def.fitsPerTask);
    CHECK(h.batch.tiers == BatchSettings::defaultTiers());
    CHECK(h.batch.validate().isEmpty());

    // And the batch block survives a host round trip.
    RemoteHost h2 = h;
    h2.batch.maxConcurrentTasks = 12;
    CHECK(RemoteHost::fromJson(h2.toJson()).batch.maxConcurrentTasks == 12);
}

TEST_CASE("BatchSettings: validation names what is wrong")
{
    BatchSettings b;
    CHECK(b.validate().isEmpty());

    b.tiers = { { 0, "4G", "1:00:00" }, { 30, "8G", "2:00:00" } };
    CHECK(b.validate().join(' ').contains("never used"));

    b.tiers = { { 30, "4G", "1:00:00" }, { 10, "8G", "2:00:00" },
                { 0, "16G", "3:00:00" } };
    CHECK(b.validate().join(' ').contains("must increase"));

    b.tiers = { { 0, "four gigs", "soon" } };
    const QString msg = b.validate().join(' ');
    CHECK(msg.contains("not a size"));
    CHECK(msg.contains("not a time"));

    b.tiers.clear();
    CHECK(b.validate().join(' ').contains("At least one"));

    b = BatchSettings{};
    b.fitsPerTask = 0;
    CHECK_FALSE(b.validate().isEmpty());
}

} // TEST_SUITE
