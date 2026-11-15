// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "checker/spot_check_checkpoint.h"

#include "common/utf8_text.h"

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spot_checker {
namespace {

namespace bj = boost::json;

constexpr int kJsonIndentSpaces = 2;
constexpr std::size_t kSpanTupleSize = 2;
constexpr std::string_view kDefaultMode = "hotspot";
constexpr std::string_view kDefaultDominantSignal = "clean";
constexpr std::string_view kDefaultCategory = "mistranslation";
constexpr std::string_view kDefaultSeverity = "major";

bj::string_view to_bj_sv(std::string_view sv) noexcept {
    return bj::string_view(sv.data(), sv.size());
}

double sanitize_json_double(double value) noexcept {
    return std::isfinite(value) ? value : 0.0;
}

void append_indent(std::string& out, int depth) {
    const int count = depth * kJsonIndentSpaces;
    if (count > 0) {
        out.append(static_cast<std::size_t>(count), ' ');
    }
}

void pretty_format_json_value(const bj::value& val, int depth, std::string& out) {
    if (val.is_object()) {
        const bj::object& obj = val.as_object();
        if (obj.empty()) {
            out += "{}";
            return;
        }
        out += "{\n";
        std::size_t idx = 0;
        for (const auto& kv : obj) {
            append_indent(out, depth + 1);
            out += bj::serialize(bj::value(kv.key()));
            out += ": ";
            pretty_format_json_value(kv.value(), depth + 1, out);
            ++idx;
            if (idx < obj.size()) {
                out.push_back(',');
            }
            out.push_back('\n');
        }
        append_indent(out, depth);
        out.push_back('}');
        return;
    }

    if (val.is_array()) {
        const bj::array& arr = val.as_array();
        if (arr.empty()) {
            out += "[]";
            return;
        }
        out += "[\n";
        for (std::size_t i = 0; i < arr.size(); ++i) {
            append_indent(out, depth + 1);
            pretty_format_json_value(arr[i], depth + 1, out);
            if (i + 1 < arr.size()) {
                out.push_back(',');
            }
            out.push_back('\n');
        }
        append_indent(out, depth);
        out.push_back(']');
        return;
    }

    out += bj::serialize(val);
}

bool read_non_negative_size(const bj::value& val, std::size_t& out) noexcept {
    constexpr std::uint64_t kMaxSize =
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    if (val.is_int64()) {
        const std::int64_t v = val.as_int64();
        if (v < 0 || static_cast<std::uint64_t>(v) > kMaxSize) {
            return false;
        }
        out = static_cast<std::size_t>(v);
        return true;
    }
    if (val.is_uint64()) {
        const std::uint64_t v = val.as_uint64();
        if (v > kMaxSize) {
            return false;
        }
        out = static_cast<std::size_t>(v);
        return true;
    }
    return false;
}

bool read_non_negative_int64(const bj::value& val, std::int64_t& out) noexcept {
    constexpr std::uint64_t kMaxInt64 =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (val.is_int64()) {
        const std::int64_t v = val.as_int64();
        if (v < 0) {
            return false;
        }
        out = v;
        return true;
    }
    if (val.is_uint64()) {
        const std::uint64_t v = val.as_uint64();
        if (v > kMaxInt64) {
            return false;
        }
        out = static_cast<std::int64_t>(v);
        return true;
    }
    return false;
}

bool read_exact_int(const bj::value& val, int& out) noexcept {
    constexpr std::int64_t kMinInt = std::numeric_limits<int>::min();
    constexpr std::int64_t kMaxInt = std::numeric_limits<int>::max();
    if (val.is_int64()) {
        const std::int64_t v = val.as_int64();
        if (v < kMinInt || v > kMaxInt) {
            return false;
        }
        out = static_cast<int>(v);
        return true;
    }
    if (val.is_uint64()) {
        const std::uint64_t v = val.as_uint64();
        if (v > static_cast<std::uint64_t>(kMaxInt)) {
            return false;
        }
        out = static_cast<int>(v);
        return true;
    }
    return false;
}

bool read_finite_double(const bj::value& val, double& out) noexcept {
    if (val.is_double()) {
        out = val.as_double();
        return std::isfinite(out);
    }
    if (val.is_int64()) {
        out = static_cast<double>(val.as_int64());
        return std::isfinite(out);
    }
    if (val.is_uint64()) {
        out = static_cast<double>(val.as_uint64());
        return std::isfinite(out);
    }
    return false;
}

Status parse_optional_string_field(const bj::object& obj,
                                   std::string_view key,
                                   std::string& target,
                                   bool keep_default_when_empty = false) {
    const bj::value* v = obj.if_contains(to_bj_sv(key));
    if (v == nullptr || v->is_null()) {
        return Status::ok();
    }
    if (!v->is_string()) {
        return Status::error(ErrorCode::CorruptCheckpoint,
                             "Checkpoint field '" + std::string(key) +
                                 "' must be a string");
    }
    const bj::string& s = v->as_string();
    if (keep_default_when_empty && s.empty()) {
        return Status::ok();
    }
    target.assign(s.c_str(), s.size());
    return Status::ok();
}

Status parse_optional_size_field(const bj::object& obj,
                                 std::string_view key,
                                 std::size_t& target) {
    const bj::value* v = obj.if_contains(to_bj_sv(key));
    if (v == nullptr || v->is_null()) {
        return Status::ok();
    }
    if (!read_non_negative_size(*v, target)) {
        return Status::error(ErrorCode::CorruptCheckpoint,
                             "Checkpoint field '" + std::string(key) +
                                 "' must be a non-negative integer");
    }
    return Status::ok();
}

Status parse_optional_int64_field(const bj::object& obj,
                                  std::string_view key,
                                  std::int64_t& target) {
    const bj::value* v = obj.if_contains(to_bj_sv(key));
    if (v == nullptr || v->is_null()) {
        return Status::ok();
    }
    if (!read_non_negative_int64(*v, target)) {
        return Status::error(ErrorCode::CorruptCheckpoint,
                             "Checkpoint field '" + std::string(key) +
                                 "' must be a non-negative integer");
    }
    return Status::ok();
}

Status parse_optional_double_field(const bj::object& obj,
                                   std::string_view key,
                                   double& target) {
    const bj::value* v = obj.if_contains(to_bj_sv(key));
    if (v == nullptr || v->is_null()) {
        return Status::ok();
    }
    if (!read_finite_double(*v, target)) {
        return Status::error(ErrorCode::CorruptCheckpoint,
                             "Checkpoint field '" + std::string(key) +
                                 "' must be a finite number");
    }
    return Status::ok();
}

Status parse_optional_bool_field(const bj::object& obj,
                                 std::string_view key,
                                 bool& target) {
    const bj::value* v = obj.if_contains(to_bj_sv(key));
    if (v == nullptr || v->is_null()) {
        return Status::ok();
    }
    if (!v->is_bool()) {
        return Status::error(ErrorCode::CorruptCheckpoint,
                             "Checkpoint field '" + std::string(key) +
                                 "' must be a boolean");
    }
    target = v->as_bool();
    return Status::ok();
}

Status parse_token_usage(const bj::object& root, TokenUsageSummary& out) {
    const bj::value* v = root.if_contains("token_usage");
    if (v == nullptr || v->is_null()) {
        return Status::ok();
    }
    if (!v->is_object()) {
        return Status::error(ErrorCode::CorruptCheckpoint,
                             "Checkpoint field 'token_usage' must be an object");
    }
    const bj::object& obj = v->as_object();
    Status st = parse_optional_size_field(obj, "api_calls", out.api_calls);
    if (!st.is_ok()) {
        return st;
    }
    st = parse_optional_size_field(obj, "radar_calls_made", out.radar_calls_made);
    if (!st.is_ok()) {
        return st;
    }
    st = parse_optional_size_field(
        obj, "repair_calls_made", out.repair_calls_made);
    if (!st.is_ok()) {
        return st;
    }
    st = parse_optional_int64_field(obj, "prompt_tokens", out.prompt_tokens);
    if (!st.is_ok()) {
        return st;
    }
    st = parse_optional_int64_field(obj, "output_tokens", out.output_tokens);
    if (!st.is_ok()) {
        return st;
    }
    return parse_optional_int64_field(obj, "thought_tokens", out.thought_tokens);
}

Status parse_offset_array(const bj::object& root,
                          std::string_view key,
                          std::vector<std::size_t>& out) {
    const bj::value* v = root.if_contains(to_bj_sv(key));
    if (v == nullptr || v->is_null()) {
        return Status::ok();
    }
    if (!v->is_array()) {
        return Status::error(
            ErrorCode::CorruptCheckpoint,
            "Checkpoint field '" + std::string(key) + "' must be an array");
    }
    const bj::array& arr = v->as_array();
    out.reserve(arr.size());
    for (const bj::value& elem : arr) {
        std::size_t off = 0;
        if (!read_non_negative_size(elem, off)) {
            return Status::error(
                ErrorCode::CorruptCheckpoint,
                "Invalid element inside '" + std::string(key) + "'");
        }
        out.push_back(off);
    }
    return Status::ok();
}

Status parse_single_span(const bj::value& elem,
                         std::string_view field_name,
                         std::pair<std::size_t, std::size_t>& out) {
    if (elem.is_array()) {
        const bj::array& pair_arr = elem.as_array();
        if (pair_arr.size() != kSpanTupleSize) {
            return Status::error(
                ErrorCode::CorruptCheckpoint,
                "Array element in '" + std::string(field_name) +
                    "' must have length 2");
        }
        std::size_t first = 0;
        std::size_t second = 0;
        if (!read_non_negative_size(pair_arr[0], first) ||
            !read_non_negative_size(pair_arr[1], second)) {
            return Status::error(
                ErrorCode::CorruptCheckpoint,
                "Invalid integer in '" + std::string(field_name) + "' tuple");
        }
        out = {first, second};
        return Status::ok();
    }

    if (!elem.is_object()) {
        return Status::error(
            ErrorCode::CorruptCheckpoint,
            "Element in '" + std::string(field_name) +
                "' must be an object or 2-item array");
    }
    const bj::object& obj = elem.as_object();
    const bj::value* off_v = obj.if_contains("offset");
    const bj::value* len_v = obj.if_contains("length");
    if (off_v != nullptr && len_v != nullptr) {
        std::size_t off = 0;
        std::size_t len = 0;
        if (!read_non_negative_size(*off_v, off) ||
            !read_non_negative_size(*len_v, len)) {
            return Status::error(
                ErrorCode::CorruptCheckpoint,
                "Invalid offset/length in '" + std::string(field_name) + "'");
        }
        out = {off, len};
        return Status::ok();
    }

    const bj::value* begin_v = obj.if_contains("begin");
    if (begin_v == nullptr) {
        begin_v = obj.if_contains("start");
    }
    const bj::value* end_v = obj.if_contains("end");
    if (begin_v != nullptr && end_v != nullptr) {
        std::size_t begin = 0;
        std::size_t end = 0;
        if (!read_non_negative_size(*begin_v, begin) ||
            !read_non_negative_size(*end_v, end) || end < begin) {
            return Status::error(
                ErrorCode::CorruptCheckpoint,
                "Invalid begin/end in '" + std::string(field_name) + "'");
        }
        out = {begin, end - begin};
        return Status::ok();
    }

    return Status::error(
        ErrorCode::CorruptCheckpoint,
        "Span object in '" + std::string(field_name) +
            "' missing offset/length");
}

Status parse_span_array(
    const bj::object& root,
    std::string_view key,
    const std::vector<std::size_t>& fallback_offsets,
    std::size_t default_window_size,
    std::vector<std::pair<std::size_t, std::size_t>>& out) {
    const bj::value* v = root.if_contains(to_bj_sv(key));
    if (v == nullptr || v->is_null()) {
        out.reserve(fallback_offsets.size());
        for (std::size_t off : fallback_offsets) {
            out.emplace_back(off, default_window_size);
        }
        return Status::ok();
    }
    if (!v->is_array()) {
        return Status::error(
            ErrorCode::CorruptCheckpoint,
            "Checkpoint field '" + std::string(key) + "' must be an array");
    }
    const bj::array& arr = v->as_array();
    out.reserve(arr.size());
    for (const bj::value& elem : arr) {
        std::pair<std::size_t, std::size_t> span{0, 0};
        const Status st = parse_single_span(elem, key, span);
        if (!st.is_ok()) {
            return st;
        }
        out.push_back(span);
    }
    return Status::ok();
}

Status parse_radar_results(const bj::object& root,
                           std::vector<RadarPassageResult>& out) {
    const bj::value* v = root.if_contains("radar_results");
    if (v == nullptr || v->is_null()) {
        return Status::ok();
    }
    if (!v->is_array()) {
        return Status::error(
            ErrorCode::CorruptCheckpoint,
            "Checkpoint field 'radar_results' must be an array");
    }
    const bj::array& arr = v->as_array();
    out.reserve(arr.size());
    for (const bj::value& elem : arr) {
        if (!elem.is_object()) {
            return Status::error(
                ErrorCode::CorruptCheckpoint,
                "Element inside 'radar_results' must be an object");
        }
        const bj::object& obj = elem.as_object();
        RadarPassageResult rpr;
        if (const bj::value* pv = obj.if_contains("passage_id");
            pv != nullptr && !pv->is_null() && !read_exact_int(*pv, rpr.passage_id)) {
            return Status::error(ErrorCode::CorruptCheckpoint,
                                 "Invalid 'passage_id' in 'radar_results'");
        }
        Status st = parse_optional_size_field(obj, "offset", rpr.offset);
        if (!st.is_ok()) {
            return st;
        }
        st = parse_optional_size_field(obj, "length", rpr.length);
        if (!st.is_ok()) {
            return st;
        }
        if (const bj::value* cv = obj.if_contains("critical_count");
            cv != nullptr && !cv->is_null() && !read_exact_int(*cv, rpr.critical_count)) {
            return Status::error(ErrorCode::CorruptCheckpoint,
                                 "Invalid 'critical_count' in 'radar_results'");
        }
        if (const bj::value* mv = obj.if_contains("major_count");
            mv != nullptr && !mv->is_null() && !read_exact_int(*mv, rpr.major_count)) {
            return Status::error(ErrorCode::CorruptCheckpoint,
                                 "Invalid 'major_count' in 'radar_results'");
        }
        if (const bj::value* mnv = obj.if_contains("minor_count");
            mnv != nullptr && !mnv->is_null() && !read_exact_int(*mnv, rpr.minor_count)) {
            return Status::error(ErrorCode::CorruptCheckpoint,
                                 "Invalid 'minor_count' in 'radar_results'");
        }
        st = parse_optional_double_field(obj, "mqm_score", rpr.mqm_score);
        if (!st.is_ok()) {
            return st;
        }
        if (const bj::value* sv = obj.if_contains("suspect_ids");
            sv != nullptr && !sv->is_null()) {
            if (!sv->is_array()) {
                return Status::error(ErrorCode::CorruptCheckpoint,
                                     "Field 'suspect_ids' must be an array");
            }
            for (const bj::value& id_v : sv->as_array()) {
                int sid = 0;
                if (!read_exact_int(id_v, sid)) {
                    return Status::error(ErrorCode::CorruptCheckpoint,
                                         "Invalid id in 'suspect_ids'");
                }
                if (sid >= 1) {
                    rpr.suspect_ids.insert(sid);
                }
            }
        }
        out.push_back(std::move(rpr));
    }
    return Status::ok();
}

Status parse_ranked_hotspots(const bj::object& root,
                             std::vector<HotspotWindow>& out) {
    const bj::value* v = root.if_contains("ranked_hotspots");
    if (v == nullptr || v->is_null()) {
        return Status::ok();
    }
    if (!v->is_array()) {
        return Status::error(
            ErrorCode::CorruptCheckpoint,
            "Checkpoint field 'ranked_hotspots' must be an array");
    }
    const bj::array& arr = v->as_array();
    out.reserve(arr.size());
    for (const bj::value& elem : arr) {
        if (!elem.is_object()) {
            return Status::error(
                ErrorCode::CorruptCheckpoint,
                "Element inside 'ranked_hotspots' must be an object");
        }
        const bj::object& obj = elem.as_object();
        HotspotWindow hw;
        Status st = parse_optional_size_field(obj, "offset", hw.offset);
        if (!st.is_ok()) {
            return st;
        }
        st = parse_optional_size_field(obj, "length", hw.length);
        if (!st.is_ok()) {
            return st;
        }
        st = parse_optional_size_field(obj, "peak_index", hw.peak_index);
        if (!st.is_ok()) {
            return st;
        }
        st = parse_optional_double_field(obj, "energy", hw.energy);
        if (!st.is_ok()) {
            return st;
        }
        st = parse_optional_double_field(obj, "cusum_peak", hw.cusum_peak);
        if (!st.is_ok()) {
            return st;
        }
        st = parse_optional_double_field(obj, "z_score", hw.z_score);
        if (!st.is_ok()) {
            return st;
        }
        st = parse_optional_double_field(
            obj, "peak_line_score", hw.peak_line_score);
        if (!st.is_ok()) {
            return st;
        }
        st = parse_optional_string_field(
            obj, "dominant_signal", hw.dominant_signal, true);
        if (!st.is_ok()) {
            return st;
        }
        out.push_back(std::move(hw));
    }
    return Status::ok();
}

Status parse_improvements(const bj::object& root,
                          std::vector<Improvement>& out) {
    const bj::value* v = root.if_contains("improvements");
    if (v == nullptr || v->is_null()) {
        return Status::ok();
    }
    if (!v->is_array()) {
        return Status::error(
            ErrorCode::CorruptCheckpoint,
            "Checkpoint field 'improvements' must be an array");
    }
    const bj::array& arr = v->as_array();
    out.reserve(arr.size());
    for (const bj::value& elem : arr) {
        if (!elem.is_object()) {
            return Status::error(
                ErrorCode::CorruptCheckpoint,
                "Element inside 'improvements' must be an object");
        }
        const bj::object& obj = elem.as_object();
        Improvement imp;
        Status st = parse_optional_size_field(
            obj, "global_index", imp.global_index);
        if (!st.is_ok()) {
            return st;
        }
        st = parse_optional_size_field(obj, "entry_index", imp.entry_index);
        if (!st.is_ok()) {
            return st;
        }
        st = parse_optional_string_field(obj, "script", imp.script_name);
        if (!st.is_ok()) {
            return st;
        }
        if (imp.script_name.empty()) {
            st = parse_optional_string_field(
                obj, "script_name", imp.script_name);
            if (!st.is_ok()) {
                return st;
            }
        }
        st = parse_optional_string_field(obj, "speaker", imp.speaker);
        if (!st.is_ok()) {
            return st;
        }
        st = parse_optional_string_field(obj, "jp", imp.jp_text);
        if (!st.is_ok()) {
            return st;
        }
        if (imp.jp_text.empty()) {
            st = parse_optional_string_field(obj, "jp_text", imp.jp_text);
            if (!st.is_ok()) {
                return st;
            }
        }
        st = parse_optional_string_field(obj, "old_en", imp.old_en);
        if (!st.is_ok()) {
            return st;
        }
        st = parse_optional_string_field(obj, "new_en", imp.improved_en);
        if (!st.is_ok()) {
            return st;
        }
        if (imp.improved_en.empty()) {
            st = parse_optional_string_field(
                obj, "improved_en", imp.improved_en);
            if (!st.is_ok()) {
                return st;
            }
        }
        st = parse_optional_string_field(
            obj, "category", imp.category, true);
        if (!st.is_ok()) {
            return st;
        }
        st = parse_optional_string_field(
            obj, "severity", imp.severity, true);
        if (!st.is_ok()) {
            return st;
        }
        st = parse_optional_string_field(obj, "error_span", imp.error_span);
        if (!st.is_ok()) {
            return st;
        }
        st = parse_optional_string_field(obj, "fix_span", imp.fix_span);
        if (!st.is_ok()) {
            return st;
        }
        st = parse_optional_double_field(obj, "confidence", imp.confidence);
        if (!st.is_ok()) {
            return st;
        }
        st = parse_optional_string_field(obj, "reason", imp.reason);
        if (!st.is_ok()) {
            return st;
        }
        if (!imp.improved_en.empty()) {
            out.push_back(std::move(imp));
        }
    }
    return Status::ok();
}

}  // namespace

Result<CheckpointState> parse_checkpoint_json(std::string_view raw_json,
                                              std::size_t default_window_size) {
    const std::string_view trimmed = trim_ascii_view(raw_json);
    if (trimmed.empty()) {
        return Result<CheckpointState>::failure(
            ErrorCode::CorruptCheckpoint,
            "Checkpoint JSON is empty");
    }

    boost::system::error_code ec;
    bj::value parsed = bj::parse(to_bj_sv(trimmed), ec);
    if (ec) {
        return Result<CheckpointState>::failure(
            ErrorCode::CorruptCheckpoint,
            "Malformed checkpoint JSON: " + ec.message());
    }
    if (!parsed.is_object()) {
        return Result<CheckpointState>::failure(
            ErrorCode::CorruptCheckpoint,
            "Checkpoint JSON root must be an object");
    }

    const bj::object& root = parsed.as_object();
    CheckpointState state;

    Status st = parse_optional_string_field(root, "game_dir", state.game_dir);
    if (!st.is_ok()) {
        return Result<CheckpointState>::failure(std::move(st));
    }
    st = parse_optional_string_field(root, "model", state.model, true);
    if (!st.is_ok()) {
        return Result<CheckpointState>::failure(std::move(st));
    }
    st = parse_optional_string_field(root, "mode", state.mode, true);
    if (!st.is_ok()) {
        return Result<CheckpointState>::failure(std::move(st));
    }

    std::size_t ignored_improvements_count = 0;
    const std::pair<std::string_view, std::size_t*> size_fields[] = {
        {"total_entries", &state.total_entries},
        {"hotspots_detected", &state.hotspots_detected},
        {"h_windows_skipped", &state.h_windows_skipped},
        {"radar_calls_made", &state.radar_calls_made},
        {"radar_passages_triaged", &state.radar_passages_triaged},
        {"adaptive_expansions", &state.adaptive_expansions},
        {"windows_checked", &state.windows_checked},
        {"lines_checked", &state.lines_checked},
        {"windows_blocked", &state.windows_blocked},
        {"lines_blocked", &state.lines_blocked},
        {"editable_lines_unlocked", &state.editable_lines_unlocked},
        {"candidates_rejected_by_validator",
         &state.candidates_rejected_by_validator},
        {"improvements_count", &ignored_improvements_count},
        {"script_entries_updated", &state.script_entries_updated},
        {"cache_entries_updated", &state.cache_entries_updated},
    };
    for (const auto& [key, field_ptr] : size_fields) {
        st = parse_optional_size_field(root, key, *field_ptr);
        if (!st.is_ok()) {
            return Result<CheckpointState>::failure(std::move(st));
        }
    }

    st = parse_optional_double_field(root, "ols_slope", state.ols_slope);
    if (!st.is_ok()) {
        return Result<CheckpointState>::failure(std::move(st));
    }
    st = parse_optional_double_field(
        root, "ols_intercept", state.ols_intercept);
    if (!st.is_ok()) {
        return Result<CheckpointState>::failure(std::move(st));
    }
    st = parse_optional_double_field(root, "ols_sigma", state.ols_sigma);
    if (!st.is_ok()) {
        return Result<CheckpointState>::failure(std::move(st));
    }
    st = parse_optional_bool_field(root, "ols_valid", state.ols_valid);
    if (!st.is_ok()) {
        return Result<CheckpointState>::failure(std::move(st));
    }

    st = parse_token_usage(root, state.token_usage);
    if (!st.is_ok()) {
        return Result<CheckpointState>::failure(std::move(st));
    }
    st = parse_offset_array(root, "checked_offsets", state.checked_offsets);
    if (!st.is_ok()) {
        return Result<CheckpointState>::failure(std::move(st));
    }
    st = parse_span_array(root,
                          "checked_spans",
                          state.checked_offsets,
                          default_window_size,
                          state.checked_spans);
    if (!st.is_ok()) {
        return Result<CheckpointState>::failure(std::move(st));
    }
    st = parse_offset_array(root, "blocked_offsets", state.blocked_offsets);
    if (!st.is_ok()) {
        return Result<CheckpointState>::failure(std::move(st));
    }
    st = parse_span_array(root,
                          "blocked_spans",
                          state.blocked_offsets,
                          default_window_size,
                          state.blocked_spans);
    if (!st.is_ok()) {
        return Result<CheckpointState>::failure(std::move(st));
    }
    st = parse_radar_results(root, state.radar_results);
    if (!st.is_ok()) {
        return Result<CheckpointState>::failure(std::move(st));
    }
    st = parse_ranked_hotspots(root, state.ranked_hotspots);
    if (!st.is_ok()) {
        return Result<CheckpointState>::failure(std::move(st));
    }
    st = parse_improvements(root, state.improvements);
    if (!st.is_ok()) {
        return Result<CheckpointState>::failure(std::move(st));
    }

    return Result<CheckpointState>::success(std::move(state));
}

std::string serialize_checkpoint_json(const CheckpointState& state) {
    bj::object root;
    root["game_dir"] = to_bj_sv(state.game_dir);
    root["model"] = to_bj_sv(state.model.empty()
                                 ? kDefaultArgonModel
                                 : std::string_view(state.model));
    root["mode"] = to_bj_sv(state.mode.empty()
                                ? kDefaultMode
                                : std::string_view(state.mode));
    root["total_entries"] = static_cast<std::int64_t>(state.total_entries);
    root["ols_slope"] = sanitize_json_double(state.ols_slope);
    root["ols_intercept"] = sanitize_json_double(state.ols_intercept);
    root["ols_sigma"] = sanitize_json_double(state.ols_sigma);
    root["ols_valid"] = state.ols_valid;
    root["hotspots_detected"] =
        static_cast<std::int64_t>(state.hotspots_detected);
    root["h_windows_skipped"] =
        static_cast<std::int64_t>(state.h_windows_skipped);
    root["radar_calls_made"] =
        static_cast<std::int64_t>(state.radar_calls_made);
    root["radar_passages_triaged"] =
        static_cast<std::int64_t>(state.radar_passages_triaged);
    root["adaptive_expansions"] =
        static_cast<std::int64_t>(state.adaptive_expansions);
    root["windows_checked"] = static_cast<std::int64_t>(state.windows_checked);
    root["lines_checked"] = static_cast<std::int64_t>(state.lines_checked);
    root["windows_blocked"] = static_cast<std::int64_t>(state.windows_blocked);
    root["lines_blocked"] = static_cast<std::int64_t>(state.lines_blocked);
    root["editable_lines_unlocked"] =
        static_cast<std::int64_t>(state.editable_lines_unlocked);
    root["candidates_rejected_by_validator"] =
        static_cast<std::int64_t>(state.candidates_rejected_by_validator);
    root["improvements_count"] =
        static_cast<std::int64_t>(state.improvements.size());
    root["script_entries_updated"] =
        static_cast<std::int64_t>(state.script_entries_updated);
    root["cache_entries_updated"] =
        static_cast<std::int64_t>(state.cache_entries_updated);

    bj::object usage_obj;
    usage_obj["api_calls"] =
        static_cast<std::int64_t>(state.token_usage.api_calls);
    usage_obj["radar_calls_made"] =
        static_cast<std::int64_t>(state.token_usage.radar_calls_made);
    usage_obj["repair_calls_made"] =
        static_cast<std::int64_t>(state.token_usage.repair_calls_made);
    usage_obj["prompt_tokens"] = state.token_usage.prompt_tokens;
    usage_obj["output_tokens"] = state.token_usage.output_tokens;
    usage_obj["thought_tokens"] = state.token_usage.thought_tokens;
    root["token_usage"] = std::move(usage_obj);

    bj::array offsets_arr;
    offsets_arr.reserve(state.checked_offsets.size());
    for (std::size_t off : state.checked_offsets) {
        offsets_arr.push_back(static_cast<std::int64_t>(off));
    }
    root["checked_offsets"] = std::move(offsets_arr);

    bj::array spans_arr;
    spans_arr.reserve(state.checked_spans.size());
    for (const auto& [off, len] : state.checked_spans) {
        bj::object span_obj;
        span_obj["offset"] = static_cast<std::int64_t>(off);
        span_obj["length"] = static_cast<std::int64_t>(len);
        spans_arr.push_back(std::move(span_obj));
    }
    root["checked_spans"] = std::move(spans_arr);

    bj::array blocked_offsets_arr;
    blocked_offsets_arr.reserve(state.blocked_offsets.size());
    for (std::size_t off : state.blocked_offsets) {
        blocked_offsets_arr.push_back(static_cast<std::int64_t>(off));
    }
    root["blocked_offsets"] = std::move(blocked_offsets_arr);

    bj::array blocked_spans_arr;
    blocked_spans_arr.reserve(state.blocked_spans.size());
    for (const auto& [off, len] : state.blocked_spans) {
        bj::object span_obj;
        span_obj["offset"] = static_cast<std::int64_t>(off);
        span_obj["length"] = static_cast<std::int64_t>(len);
        blocked_spans_arr.push_back(std::move(span_obj));
    }
    root["blocked_spans"] = std::move(blocked_spans_arr);

    bj::array radar_arr;
    radar_arr.reserve(state.radar_results.size());
    for (const RadarPassageResult& rpr : state.radar_results) {
        bj::array suspect_arr;
        suspect_arr.reserve(rpr.suspect_ids.size());
        for (int sid : rpr.suspect_ids) {
            suspect_arr.push_back(sid);
        }
        bj::object r_obj;
        r_obj["passage_id"] = rpr.passage_id;
        r_obj["offset"] = static_cast<std::int64_t>(rpr.offset);
        r_obj["length"] = static_cast<std::int64_t>(rpr.length);
        r_obj["critical_count"] = rpr.critical_count;
        r_obj["major_count"] = rpr.major_count;
        r_obj["minor_count"] = rpr.minor_count;
        r_obj["mqm_score"] = sanitize_json_double(rpr.mqm_score);
        r_obj["suspect_ids"] = std::move(suspect_arr);
        radar_arr.push_back(std::move(r_obj));
    }
    root["radar_results"] = std::move(radar_arr);

    bj::array hotspots_arr;
    hotspots_arr.reserve(state.ranked_hotspots.size());
    for (const HotspotWindow& hw : state.ranked_hotspots) {
        bj::object hw_obj;
        hw_obj["offset"] = static_cast<std::int64_t>(hw.offset);
        hw_obj["length"] = static_cast<std::int64_t>(hw.length);
        hw_obj["energy"] = sanitize_json_double(hw.energy);
        hw_obj["z_score"] = sanitize_json_double(hw.z_score);
        hw_obj["cusum_peak"] = sanitize_json_double(hw.cusum_peak);
        hw_obj["peak_index"] = static_cast<std::int64_t>(hw.peak_index);
        hw_obj["peak_line_score"] = sanitize_json_double(hw.peak_line_score);
        hw_obj["dominant_signal"] = to_bj_sv(
            hw.dominant_signal.empty()
                ? kDefaultDominantSignal
                : std::string_view(hw.dominant_signal));
        hotspots_arr.push_back(std::move(hw_obj));
    }
    root["ranked_hotspots"] = std::move(hotspots_arr);

    bj::array imp_arr;
    imp_arr.reserve(state.improvements.size());
    for (const Improvement& imp : state.improvements) {
        bj::object imp_obj;
        imp_obj["global_index"] = static_cast<std::int64_t>(imp.global_index);
        imp_obj["script"] = to_bj_sv(imp.script_name);
        imp_obj["entry_index"] = static_cast<std::int64_t>(imp.entry_index);
        imp_obj["speaker"] = to_bj_sv(imp.speaker);
        imp_obj["jp"] = to_bj_sv(imp.jp_text);
        imp_obj["old_en"] = to_bj_sv(imp.old_en);
        imp_obj["improved_en"] = to_bj_sv(imp.improved_en);
        imp_obj["new_en"] = to_bj_sv(imp.improved_en);
        imp_obj["error_span"] = to_bj_sv(imp.error_span);
        imp_obj["fix_span"] = to_bj_sv(imp.fix_span);
        imp_obj["severity"] = to_bj_sv(
            imp.severity.empty() ? kDefaultSeverity
                                 : std::string_view(imp.severity));
        imp_obj["category"] = to_bj_sv(
            imp.category.empty() ? kDefaultCategory
                                 : std::string_view(imp.category));
        imp_obj["confidence"] = sanitize_json_double(imp.confidence);
        imp_obj["reason"] = to_bj_sv(imp.reason);
        imp_arr.push_back(std::move(imp_obj));
    }
    root["improvements"] = std::move(imp_arr);

    std::string formatted;
    pretty_format_json_value(bj::value(std::move(root)), 0, formatted);
    formatted.push_back('\n');
    return formatted;
}

Result<std::optional<CheckpointState>> load_checkpoint_if_exists(
    const IFileStore& store,
    const std::filesystem::path& path,
    std::size_t default_window_size) {
    if (!store.exists(path)) {
        return Result<std::optional<CheckpointState>>::success(std::nullopt);
    }

    Result<std::string> raw = store.read_text(path);
    if (!raw.is_ok()) {
        return Result<std::optional<CheckpointState>>::failure(raw.status());
    }

    Result<CheckpointState> parsed =
        parse_checkpoint_json(raw.value(), default_window_size);
    if (!parsed.is_ok()) {
        return Result<std::optional<CheckpointState>>::failure(parsed.status());
    }

    return Result<std::optional<CheckpointState>>::success(
        std::optional<CheckpointState>(std::move(parsed).value()));
}

Status save_checkpoint_atomic(IFileStore& store,
                              const std::filesystem::path& path,
                              const CheckpointState& state) {
    return store.write_text_atomic(path, serialize_checkpoint_json(state));
}

}  // namespace spot_checker
