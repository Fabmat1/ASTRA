#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace astra::remote {

/*  How a host runs many fits at once for the mass fitter.
 *
 *  A mass fit on a cluster cannot submit one job per fit: a single spectrum
 *  fits in well under a minute, a campaign has tens of thousands of them, and
 *  every cluster caps how many jobs one user may have queued.  Fits are
 *  therefore packed into batches, each batch runs as one task (a Slurm array
 *  element, or a detached runner on a plain host), and every knob that
 *  depends on the site - memory, wall time, how many tasks may run at once,
 *  how large an array may be - is a setting of the host, never a constant.
 *  The defaults are deliberately conservative starting points; the Detect
 *  button in the settings page fills the scheduler limits from the cluster
 *  itself, and measured usage can suggest better tiers once fits have run.
 *
 *  Memory and time are chosen per *tier*: a star is placed in the first tier
 *  whose `maxSpectra` covers its number of spectra, since a joint fit's
 *  memory grows with the spectra it holds.  A fit killed for running out of
 *  memory or time is retried one tier up.                                   */
struct ResourceTier {
    /*  Upper bound on spectra per fit for this tier; 0 = unbounded, which
     *  only makes sense for the last tier.                                  */
    int     maxSpectra = 0;
    /*  Memory for one task, in Slurm's size syntax ("4G", "500M").  On a
     *  plain host it caps the worker instead (GAEL --mem-budget).           */
    QString memory;
    /*  Wall time for one task, in Slurm's syntax ("05:00:00", "1-00:00:00").
     *  Also bounds how many fits are packed into one task.                  */
    QString timeLimit;

    QJsonObject toJson() const;
    static ResourceTier fromJson(const QJsonObject& o);
    bool operator==(const ResourceTier&) const = default;
};

struct BatchSettings {
    /*  Pack mass-fit work into batches on this host.  Off, every fit is a
     *  job of its own, which is only sensible for a handful of stars.       */
    bool enabled = true;

    /*  Upper bound on fits per task.  Tasks are also cut short of their
     *  tier's time limit using measured fit durations, whichever comes
     *  first.                                                               */
    int fitsPerTask = 20;

    /*  Threads one fit uses.  Mass fitting parallelises over fits, so 1 is
     *  normally best; more only helps very large joint fits.                */
    int cpusPerTask = 1;

    /*  Slurm: largest array one submission may carry; 0 = take the
     *  cluster's MaxArraySize (detected), else 1000.                        */
    int maxArraySize = 0;

    /*  How many tasks may run at the same time (Slurm's `--array=...%N`, or
     *  parallel runners on a plain host); 0 = no limit beyond the
     *  scheduler's own.  On a shared cluster this is the knob that keeps a
     *  campaign within the user's QoS.                                      */
    int maxConcurrentTasks = 0;

    /*  Slurm: most tasks this host may have queued or running at once
     *  across all batches; 0 = no limit.  Keeps ASTRA under per-user submit
     *  limits (MaxSubmitJobs).                                              */
    int maxQueuedTasks = 0;

    /*  A task stops starting new fits when less than this is left of its
     *  wall time, on top of the next fit's expected duration; the rest go
     *  back to be rebatched.                                                */
    int timeGuardSeconds = 300;

    /*  Retries per fit after an out-of-memory or out-of-time kill, each one
     *  tier up.  A fit that fails in the last tier fails for good.          */
    int maxRetries = 2;

    /*  Remember the memory and time each fit really used on this host, so
     *  the settings page can suggest tiers and packing can estimate task
     *  durations.                                                           */
    bool learnFromUsage = true;

    /*  Ordered by maxSpectra ascending; the last one should be unbounded.   */
    QVector<ResourceTier> tiers = defaultTiers();

    QJsonObject toJson() const;
    static BatchSettings fromJson(const QJsonObject& o);

    static QVector<ResourceTier> defaultTiers();

    /*  Index of the tier a fit of `nSpectra` spectra belongs in: the first
     *  whose bound covers it, else the last.  -1 when there are no tiers.   */
    int tierFor(int nSpectra) const;

    /*  The tier to retry in after a kill in `tier`, or -1 when `tier` is
     *  already the last.                                                    */
    int escalate(int tier) const;

    /*  Problems that make the settings unusable, for the settings page.    */
    QStringList validate() const;
};

/*  "4G" / "512M" / "1T" / plain bytes -> bytes; -1 when unparsable.  Slurm's
 *  default unit (a bare number) is megabytes, which is what `bareIsMegabytes`
 *  selects.                                                                 */
long long parseMemorySize(const QString& s, bool bareIsMegabytes = true);

/*  Slurm time syntax -> seconds: "MM", "MM:SS", "HH:MM:SS", "D-HH",
 *  "D-HH:MM", "D-HH:MM:SS".  -1 when unparsable or empty.                   */
long long parseSlurmTime(const QString& s);

} // namespace astra::remote
