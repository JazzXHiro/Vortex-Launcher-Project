#pragma once
#include <cstddef>
#include <string>

// Both API clients read their responses by scanning for a key and walking the
// string that follows, rather than by building a document. That is fine for the
// handful of fields Vortex wants, but each scan grew its own copy of the same
// escape handling, and all of them decoded an escape as "drop the backslash,
// keep the next character". That is right for \" \\ and \/ and wrong for
// everything else: IGDB sends an apostrophe as ', so "Hack and slash/Beat
// 'em up" reached game_metadata.txt as "Beat u0027em up", and every game with an
// apostrophe in a genre or a company name has carried the mangled form since.
//
// pos must index the opening quote of a JSON string. On return it indexes the
// closing quote, or the end of the input for an unterminated string -- which
// only a truncated response produces.
//
// \uXXXX is decoded to UTF-8, surrogate pairs included: every string here ends
// up in a std::string that Qt and std::filesystem both read as UTF-8.
std::string json_read_string(const std::string &json, std::size_t &pos);

// Repairs a value that an escape-dropping scan already wrote to a cache file.
// Those files are append-only and are never re-fetched for an id already in
// them, so fixing the parser alone would leave every existing line mangled.
//
// A dropped escape leaves no backslash behind, so what is left to find is a
// bare "u" followed by four hex digits -- a surrogate pair leaves two of them
// in a row. Nothing else is touched, and a lone surrogate is left alone rather
// than guessed at.
std::string json_repair_dropped_escapes(const std::string &value);
