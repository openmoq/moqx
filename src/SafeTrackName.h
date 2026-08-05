/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <moxygen/MoQTypes.h>

namespace openmoq::moqx {

// Renders names in the form RECOMMENDED by moq-transport, "Representing
// Namespace and Track Names".
//
// The encoding is injective, so two names never collapse onto one rendering —
// which matters wherever the result keys something (metric labels, filenames).
std::string safeName(std::string_view bytes);

std::string safeName(const moxygen::TrackNamespace& ns);

std::string safeName(const moxygen::FullTrackName& ftn);

// Inverses of the above; nullopt when the input is not in the safe form.
// Hex digits are accepted in either case, though safeName only emits lower.
std::optional<std::string> parseSafeBytes(std::string_view text);

std::optional<moxygen::TrackNamespace> parseSafeNamespace(std::string_view text);

// Splits at the last "--": an encoded track name never contains '-', so that
// separator is unambiguous even when the namespace ends in an empty tuple.
std::optional<moxygen::FullTrackName> parseSafeFullTrackName(std::string_view text);

} // namespace openmoq::moqx
