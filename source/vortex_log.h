#pragma once

#include <string>

// Structured logging to the console and to vortex.log beside the executable.
//
// The launcher is a console-subsystem binary, so it already had somewhere to
// print -- but what it printed was unstructured, interleaved from two threads,
// and block-buffered. Killing the process, or a crash mid-scan, discarded
// everything it had "printed" but not yet flushed, which is exactly the case
// you most want a log for. vlog_init() turns buffering off and opens the file.
//
// The file half exists so a user on another machine can send a log instead of
// describing a symptom. It lives beside the exe like every other Vortex data
// file (see app_paths.h) and is truncated at startup once it passes a size cap,
// so a long-lived install never grows one unbounded file.
//
// All functions are safe to call from any thread; the scan runs on a QThread
// and logs from it while the UI logs from the main thread.
namespace vlog {

// Fixed-width status column, so a scan's output can be read down the page and
// grepped for the interesting lines.
enum class Status {
    Ok,       // did the work, it succeeded
    Cached,   // already had it, no network call
    Skipped,  // deliberately not attempted (no API key, nothing to do)
    Fail,     // attempted and failed
};

// Truncates an oversized log, writes the session header, disables stdout
// buffering. Safe to call more than once; only the first call does anything.
void init();

// One line: "HH:MM:SS [category] message"
void line(const std::string &category, const std::string &message);

// A section header plus the item count the phase is about to work through.
// `total` of 0 prints the header without a count.
void phase(const std::string &name, int total = 0);

// One item within a phase, with the status column and optional trailing detail.
void item(const std::string &category, const std::string &name, Status status,
          const std::string &detail = "");

// Closing line for a phase: elapsed time and the tallies. Pass whatever of the
// counters the phase actually tracks; zeros are still printed so the shape of
// the summary is identical every time.
void phase_done(const std::string &name, double seconds, int ok, int cached,
                int skipped, int failed);

// Where the log is being written, for the "logs are here" hint. Empty if the
// file could not be opened -- console output still works in that case.
std::string file_path();

}  // namespace vlog
