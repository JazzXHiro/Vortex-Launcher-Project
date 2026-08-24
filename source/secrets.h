#pragma once

#include <string>

// Runtime secrets, read from a .env file instead of being compiled in.
//
// API keys used to live as #define literals in igdb_manager.cpp and
// steamgriddb_manager.cpp. That meant every key was committed to source
// control — and once a secret is in git history, deleting the line does not
// un-publish it; it has to be revoked at the provider. It also meant rotating
// a key required a rebuild.
//
// The same analytics/.env that the Python side already reads is used here, so
// there is exactly one place to put credentials. Lookup order:
//
//   1. $VORTEX_ENV_FILE            (explicit override, useful for CI)
//   2. <exe dir>/analytics/.env    (CMake copies analytics/ next to the exe)
//   3. <exe dir>/.env
//
// Values are cached after the first read. Returns `fallback` when the key is
// absent, so a missing file degrades to "no credentials" rather than crashing.
std::string get_secret(const std::string &key, const std::string &fallback = "");

// True when the key is present and non-empty. Use to produce one clear
// diagnostic instead of letting an empty key surface as an opaque HTTP 401.
bool has_secret(const std::string &key);

// Drop the cache and re-read the .env file on the next lookup.
//
// Needed because the first-run wizard writes analytics/.env while the process
// is already running. Without this the values loaded at startup -- an empty
// map, on a fresh install -- persist for the lifetime of the process, and the
// keys the user just typed do nothing until they restart the launcher.
void reload_secrets();
