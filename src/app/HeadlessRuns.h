#pragma once

/*  Long-running jobs without the ASTRA window, for a terminal or an ssh
 *  session on a machine that stays up. They use the same services the window
 *  does - with ASTRA's settings and database - so run them with the window
 *  closed.
 *
 *      astra --fetch-spectra <project> --archives lamost-lrs[,sdss,...]
 *            [--exposures] [--redownload] [--join-arms] [--limit N]
 *
 *  Searches the archives for every star of the project (or the first N) and
 *  imports everything found, exactly like Fetch Spectra with "download all".
 *
 *      astra --mass-fit <project> --plan <plan.json> [--limit N]
 *            [--existing add|skip|poor] [--parallel N]
 *      astra --mass-fit-resume <run id>
 *
 *  Imports the plan into the project (as the manager's Import does) and runs
 *  it over the project's stars, or resumes an interrupted run. Remote fits on
 *  batching hosts keep running if this process is stopped; resume collects
 *  them.
 *
 *  Progress is printed every 30 s. Returns -1 when argv is not one of these. */
int runHeadless(int argc, char** argv);
