// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// cg_parity -- verification-only driver.
//
// Exposes the two deterministic halves of the narrative-CG tool (the DDP entry
// table and the repacker) so a rebuilt archive can be byte-compared against a
// reference one without running a scan or making a single API call.  Not part
// of the pipeline; the verification harness in analysys/ is its only caller.
//
//   cg_parity list   <archive>
//   cg_parity repack <archive> <out> <manifest.json>
//
// The manifest is [{"entry": "...", "bmp": "..."}, ...] in the order the
// replacements should be applied.
#include "apps/paths.h"
#include "common/util.h"
#include "ddp/ddp_archive.h"

#include <boost/json.hpp>

#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace bj = boost::json;

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr std::size_t kCommandArgIndex = 0;
constexpr std::size_t kArchiveArgIndex = 1;
constexpr std::size_t kOutputArchiveArgIndex = 2;
constexpr std::size_t kManifestArgIndex = 3;
constexpr std::size_t kListArgCount = 2;
constexpr std::size_t kRepackArgCount = 4;

}  // namespace

int main() {
    using namespace shin;
    setup_console_utf8();
    try {
        const std::vector<std::string> args = apps::process_utf8_args();
        const std::string cmd = args.size() > kCommandArgIndex ? args[kCommandArgIndex] : "";

        if (cmd == "list" && args.size() == kListArgCount) {
            bj::array out;
            for (const auto& e : ddp::list_archive(args[kArchiveArgIndex])) {
                out.push_back(bj::array{e.name, e.offset, e.unpacked, e.packed});
            }
            std::cout << json_dump(bj::value(std::move(out))) << std::endl;
            return kExitOk;
        }

        if (cmd == "repack" && args.size() == kRepackArgCount) {
            ddp::Replacements reps;
            // Bind the parsed value: in C++17 a range-for does NOT extend the
            // lifetime of a temporary the range initialiser only refers into,
            // so iterating json_parse_file(...).get_array() directly walks
            // freed memory.
            const bj::value manifest = json_parse_file(args[kManifestArgIndex]);
            for (const auto& v : manifest.get_array()) {
                const bj::object& o = v.get_object();
                reps.emplace_back(std::string(o.at("entry").get_string()),
                                  read_file(std::string(o.at("bmp").get_string())));
            }
            ddp::repack(
                args[kArchiveArgIndex], args[kOutputArchiveArgIndex], reps);
            return kExitOk;
        }

        std::cerr << "usage: cg_parity list <archive>\n"
                  << "       cg_parity repack <archive> <out> <manifest.json>\n";
        return kExitUsage;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        return kExitError;
    }
}
