#pragma once

/*  Headless maintenance, run from a terminal.
 *
 *      astra --validate-plan <plan.json>
 *
 *  Reads a mass-fitting plan as exported from the manager (or written by
 *  hand), prints its tree and what would stop it from running.  Touches no
 *  database.
 *
 *  Returns -1 when argv is not a maintenance invocation.                    */
int runMaintenanceMode(int argc, char** argv);
