/*
  engine/src/main.cpp：C++ 核心引擎（demo 版，CLI + JSON 通信）

  这个文件实现了一个最小的本地“引擎”程序 engine_cli，用来给 Python agent 调用：
  - list-files：列出文件树（过滤常见大目录）
  - read-file：读取文件内容（限制最大字节数，避免上下文爆炸）
  - search-text：全文搜索（demo 版：逐文件逐行 find；后续可换索引/rg/tree-sitter）
  - apply-edits：应用“按行替换”的编辑指令，并生成快照（snapshot）
  - rollback：把快照内容写回去，实现回滚

  设计动机（答辩友好）：
  - Python 负责“编排/工作流/LLM”，C++ 负责“本地高性能/工程能力”
  - 两者用 CLI 子进程 + JSON 通信：最稳、最容易调试、跨平台也清晰
*/

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem; // C++17 文件系统库

static void print_usage(const char* argv0) { // 打印用法说明
  std::cerr  //
      << "Usage:\n"
      << "  " << argv0 << " list-files --root PATH\n"
      << "  " << argv0 << " read-file --path PATH [--max-bytes N]\n"
      << "  " << argv0
      << " search-text --root PATH --query TEXT [--topk K] [--max-bytes N]\n"
      << "  " << argv0 << " apply-edits --root PATH --edits-json PATH\n"
      << "  " << argv0 << " rollback --root PATH --snapshot-id ID\n"
      << "\n"
      << "All commands output JSON on stdout.\n";
}

static std::string json_escape(const std::string& s) {
  // 把任意字符串安全地放进 JSON 字符串字段里（最小实现，只处理常见转义）
  std::string out;
  out.reserve(s.size() + 16);
  for (unsigned char c : s) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) { // 大小 32 以下的控制字符，用 \u00XX 表示
          char buf[7]; // \uXXXX + NUL
          std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned int>(c)); // %04X是四位十六进制，输出到buf里
          out += buf;
        } else {
          out.push_back(static_cast<char>(c)); // 普通字符直接添加
        }
    }
  }
  return out;
}

static std::string to_posix_path(const fs::path& p) { // 把路径转换为 POSIX 风格（斜杠分隔）
  return p.generic_string();
}

static bool read_file_bytes(const fs::path& path, std::size_t max_bytes,
                            std::string& out) {
  // 以二进制读取文件，并截断到 max_bytes（用于控制上下文大小）
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.clear();
  out.reserve(std::min<std::size_t>(max_bytes, 1 << 20));
  char buf[8192];
  std::size_t total = 0;
  while (in && total < max_bytes) {
    std::size_t want = std::min<std::size_t>(sizeof(buf), max_bytes - total);
    in.read(buf, static_cast<std::streamsize>(want));
    std::streamsize got = in.gcount();
    if (got <= 0) break;
    out.append(buf, static_cast<std::size_t>(got));
    total += static_cast<std::size_t>(got);
  }
  return true;
}

static std::vector<std::string> split_lines(const std::string& text) { // 按行拆分文本
  std::vector<std::string> lines;
  std::string line;
  std::istringstream iss(text); // 按行读取
  while (std::getline(iss, line)) 
      lines.push_back(line);
  if (!text.empty() && text.back() == '\n') 
      lines.push_back("");
  return lines;
}

static std::string join_lines(const std::vector<std::string>& lines) { // 按行合并文本
  std::ostringstream oss;
  for (std::size_t i = 0; i < lines.size(); i++) {
    oss << lines[i];
    if (i + 1 < lines.size()) oss << "\n";
  }
  return oss.str();
}

static std::unordered_set<std::string> default_ignored_dirs() {
  // 遍历/检索时要跳过的大目录（避免浪费时间 & 避免把大量无关内容喂给模型）
  return {".git", "build", "node_modules", "dist", "__pycache__", ".venv",
          ".idea", ".vscode"};
}

static bool should_ignore(const fs::path& path) {
  // path 是 root 下的相对路径；只要任意一段目录命中忽略规则，就跳过。
  auto ignored = default_ignored_dirs();
  for (const auto& part : path) {
    std::string name = part.string();
    if (ignored.count(name) != 0) return true;
    if (name.size() >= 5 && name.rfind(".dSYM") == name.size() - 5) return true;
  }
  return false;
}

static bool is_likely_text(const std::string& bytes) {
  // 很粗糙的“是否像文本”的判断：避免把二进制文件（如 .dSYM/可执行文件）塞进 JSON 输出。
  std::size_t sample = std::min<std::size_t>(bytes.size(), 4096);
  if (sample == 0) return true;
  std::size_t suspicious = 0;
  for (std::size_t i = 0; i < sample; i++) {
    unsigned char c = static_cast<unsigned char>(bytes[i]);
    if (c == 0) return false;
    if (c < 0x09) suspicious++;
    if (c >= 0x0E && c < 0x20) suspicious++;
  }
  return suspicious * 100 / sample < 5;
}

static int cmd_list_files(const fs::path& root) {
  std::vector<std::string> files;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    const auto& entry = *it;
    fs::path rel = fs::relative(entry.path(), root, ec);
    if (ec) continue;
    if (should_ignore(rel)) {
      if (entry.is_directory(ec)) it.disable_recursion_pending();
      continue;
    }
    if (!entry.is_regular_file(ec)) continue;
    files.push_back(to_posix_path(rel));
  }
  std::sort(files.begin(), files.end());

  std::cout << "{\"ok\":true,\"root\":\"" << json_escape(to_posix_path(root))
            << "\",\"files\":[";
  for (std::size_t i = 0; i < files.size(); i++) {
    if (i) std::cout << ",";
    std::cout << "\"" << json_escape(files[i]) << "\"";
  }
  std::cout << "]}\n";
  return 0;
}

static int cmd_read_file(const fs::path& path, std::size_t max_bytes) {
  std::string bytes;
  if (!read_file_bytes(path, max_bytes, bytes)) {
    std::cout << "{\"ok\":false,\"error\":\"read_failed\",\"path\":\""
              << json_escape(to_posix_path(path)) << "\"}\n";
    return 2;
  }
  std::cout << "{\"ok\":true,\"path\":\"" << json_escape(to_posix_path(path))
            << "\",\"truncated\":" << (bytes.size() >= max_bytes ? "true" : "false")
            << ",\"content\":\"" << json_escape(bytes) << "\"}\n";
  return 0;
}

static int cmd_search_text(const fs::path& root, const std::string& query,
                           int topk, std::size_t max_bytes) {
  struct Match {
    std::string path;
    int line = 0;
    std::string snippet;
  };

  std::vector<std::pair<int, Match>> scored;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    const auto& entry = *it;
    fs::path rel = fs::relative(entry.path(), root, ec);
    if (ec) continue;
    if (should_ignore(rel)) {
      if (entry.is_directory(ec)) it.disable_recursion_pending();
      continue;
    }
    if (!entry.is_regular_file(ec)) continue;

    std::string bytes;
    if (!read_file_bytes(entry.path(), max_bytes, bytes)) continue;
    if (!is_likely_text(bytes)) continue;
    auto lines = split_lines(bytes);
    for (std::size_t i = 0; i < lines.size(); i++) {
      if (lines[i].find(query) == std::string::npos) continue;
      int score = 1000;
      score -= static_cast<int>(std::min<std::size_t>(lines[i].size(), 200));
      Match m;
      m.path = to_posix_path(rel);
      m.line = static_cast<int>(i + 1);
      m.snippet = lines[i];
      scored.push_back({score, std::move(m)});
    }
  }

  std::sort(scored.begin(), scored.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  if (topk < 1) topk = 1;
  if (static_cast<int>(scored.size()) > topk) scored.resize(topk);

  std::cout << "{\"ok\":true,\"query\":\"" << json_escape(query)
            << "\",\"results\":[";
  for (std::size_t i = 0; i < scored.size(); i++) {
    if (i) std::cout << ",";
    const auto& r = scored[i].second;
    std::cout << "{\"path\":\"" << json_escape(r.path) << "\",\"line\":"
              << r.line << ",\"snippet\":\"" << json_escape(r.snippet) << "\"}";
  }
  std::cout << "]}\n";
  return 0;
}

static std::optional<std::string> read_text_file_all(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

static bool write_text_file_all(const fs::path& path, const std::string& content,
                                std::string& err) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    err = "write_failed";
    return false;
  }
  out << content;
  if (!out.good()) {
    err = "write_failed";
    return false;
  }
  return true;
}

struct Edit {
  std::string path;
  int start_line = 0;  // 1-based inclusive
  int end_line = 0;    // 1-based inclusive
  std::string replacement;
};

static std::optional<std::vector<Edit>> parse_edits_json(const std::string& text,
                                                        std::string& err) {
  // Minimal parser for:
  // {"edits":[{"path":"...","start_line":1,"end_line":2,"replacement":"..."}]}
  // NOTE: This is intentionally tiny for the demo; later replace with a real JSON lib.
  std::vector<Edit> edits;

  std::regex edit_re(R"EDITS(\{\s*"path"\s*:\s*"([^"]*)"\s*,\s*"start_line"\s*:\s*([0-9]+)\s*,\s*"end_line"\s*:\s*([0-9]+)\s*,\s*"replacement"\s*:\s*"((?:\\.|[^"\\])*)"\s*\})EDITS");

  // 将 JSON 字符串字面量（不含外围引号）反转义成真实内容。
  //
  // 重要细节：要正确区分这两种情况：
  // - "\n"  表示换行（应该变成真正的 '\n'）
  // - "\\n" 表示两个字符：反斜杠 + n（应该保留为 "\\n"）
  //
  // 之前用简单的 regex_replace 会把 "\\n" 误处理成 "\<换行>"，导致 C++ 代码出现行续接。
  auto json_unescape = [](const std::string& in, std::string& out,
                          std::string& uerr) -> bool {
    out.clear();
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); i++) {
      char c = in[i];
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (i + 1 >= in.size()) {
        uerr = "invalid_escape_trailing_backslash";
        return false;
      }
      char n = in[++i];
      switch (n) {
        case 'n':
          out.push_back('\n');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case 'u': {
          if (i + 4 >= in.size()) {
            uerr = "invalid_unicode_escape";
            return false;
          }
          unsigned int code = 0;
          for (int k = 0; k < 4; k++) {
            char h = in[i + 1 + k];
            code <<= 4;
            if (h >= '0' && h <= '9')
              code |= static_cast<unsigned int>(h - '0');
            else if (h >= 'a' && h <= 'f')
              code |= static_cast<unsigned int>(10 + h - 'a');
            else if (h >= 'A' && h <= 'F')
              code |= static_cast<unsigned int>(10 + h - 'A');
            else {
              uerr = "invalid_unicode_escape";
              return false;
            }
          }
          i += 4;
          // demo 版：只保证 ASCII 可读；更完整的 UTF-8 编码可作为后续增强点。
          if (code <= 0x7F) {
            out.push_back(static_cast<char>(code));
          } else {
            out.push_back('?');
          }
          break;
        }
        default:
          uerr = "unsupported_escape";
          return false;
      }
    }
    return true;
  };
  auto begin = std::sregex_iterator(text.begin(), text.end(), edit_re);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    Edit e;
    e.path = (*it)[1].str();
    e.start_line = std::stoi((*it)[2].str());
    e.end_line = std::stoi((*it)[3].str());
    std::string replacement_raw = (*it)[4].str();
    std::string replacement;
    std::string uerr;
    if (!json_unescape(replacement_raw, replacement, uerr)) {
      err = "invalid_replacement_string";
      return std::nullopt;
    }
    e.replacement = std::move(replacement);
    edits.push_back(std::move(e));
  }
  if (edits.empty()) {
    err = "invalid_or_empty_edits_json";
    return std::nullopt;
  }
  return edits;
}

static std::string timestamp_id() {
  using namespace std::chrono;
  auto now = system_clock::now().time_since_epoch();
  auto ms = duration_cast<milliseconds>(now).count();
  return std::to_string(ms);
}

static int cmd_apply_edits(const fs::path& root, const fs::path& edits_json_path) {
  // apply-edits：
  // - 输入：一个 edits.json（包含若干“文件路径 + 行号区间 + replacement”）
  // - 行为：
  //   1) 读取目标文件
  //   2) 对每个文件先做快照备份到 root/.agent_snapshots/<snapshot_id>/...
  //   3) 再把指定行号区间替换成 replacement
  // - 输出：{ ok, snapshot_id, changed[] }
  //
  // 为什么要 snapshot？
  // - 这是“可控修改”的核心：任何一次自动修改都必须可回滚
  // - 答辩时你可以强调：即使模型/规则出错，也不会把仓库改坏
  auto text_opt = read_text_file_all(edits_json_path);
  if (!text_opt.has_value()) {
    std::cout << "{\"ok\":false,\"error\":\"edits_json_read_failed\"}\n";
    return 2;
  }
  std::string parse_err;
  auto edits_opt = parse_edits_json(*text_opt, parse_err);
  if (!edits_opt.has_value()) {
    std::cout << "{\"ok\":false,\"error\":\"" << json_escape(parse_err) << "\"}\n";
    return 2;
  }

  std::string snapshot_id = timestamp_id();
  fs::path snap_root = root / ".agent_snapshots" / snapshot_id;
  std::error_code ec;
  fs::create_directories(snap_root, ec);

  std::vector<std::string> changed;
  for (const auto& e : *edits_opt) {
    fs::path abs = root / fs::path(e.path);
    auto content_opt = read_text_file_all(abs);
    if (!content_opt.has_value()) {
      std::cout << "{\"ok\":false,\"error\":\"file_read_failed\",\"path\":\""
                << json_escape(e.path) << "\"}\n";
      return 2;
    }
    auto lines = split_lines(*content_opt);
    if (e.start_line < 1 || e.end_line < e.start_line ||
        e.end_line > static_cast<int>(lines.size())) {
      std::cout << "{\"ok\":false,\"error\":\"invalid_line_range\",\"path\":\""
                << json_escape(e.path) << "\"}\n";
      return 2;
    }

    // Snapshot original.
    fs::path snap_path = snap_root / fs::path(e.path);
    fs::create_directories(snap_path.parent_path(), ec);
    std::string write_err;
    if (!write_text_file_all(snap_path, *content_opt, write_err)) {
      std::cout << "{\"ok\":false,\"error\":\"snapshot_write_failed\",\"path\":\""
                << json_escape(e.path) << "\"}\n";
      return 2;
    }

    std::vector<std::string> repl_lines = split_lines(e.replacement);
    lines.erase(lines.begin() + (e.start_line - 1), lines.begin() + e.end_line);
    lines.insert(lines.begin() + (e.start_line - 1), repl_lines.begin(),
                 repl_lines.end());

    std::string updated = join_lines(lines);
    if (!write_text_file_all(abs, updated, write_err)) {
      std::cout << "{\"ok\":false,\"error\":\"write_failed\",\"path\":\""
                << json_escape(e.path) << "\"}\n";
      return 2;
    }
    changed.push_back(e.path);
  }

  std::sort(changed.begin(), changed.end());
  changed.erase(std::unique(changed.begin(), changed.end()), changed.end());

  std::cout << "{\"ok\":true,\"snapshot_id\":\"" << json_escape(snapshot_id)
            << "\",\"changed\":[";
  for (std::size_t i = 0; i < changed.size(); i++) {
    if (i) std::cout << ",";
    std::cout << "\"" << json_escape(changed[i]) << "\"";
  }
  std::cout << "]}\n";
  return 0;
}

static int cmd_rollback(const fs::path& root, const std::string& snapshot_id) {
  // rollback：
  // - 输入：snapshot_id
  // - 行为：遍历 root/.agent_snapshots/<snapshot_id>/ 下的文件，并写回 root 对应位置
  // - 输出：{ ok, snapshot_id, restored[] }
  fs::path snap_root = root / ".agent_snapshots" / snapshot_id;
  std::error_code ec;
  if (!fs::exists(snap_root, ec) || !fs::is_directory(snap_root, ec)) {
    std::cout << "{\"ok\":false,\"error\":\"snapshot_not_found\",\"snapshot_id\":\""
              << json_escape(snapshot_id) << "\"}\n";
    return 2;
  }

  std::vector<std::string> restored;
  for (auto it = fs::recursive_directory_iterator(snap_root, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    const auto& entry = *it;
    if (!entry.is_regular_file(ec)) continue;

    fs::path rel = fs::relative(entry.path(), snap_root, ec);
    if (ec) continue;
    fs::path dest = root / rel;
    fs::create_directories(dest.parent_path(), ec);

    auto content_opt = read_text_file_all(entry.path());
    if (!content_opt.has_value()) {
      std::cout << "{\"ok\":false,\"error\":\"snapshot_read_failed\",\"path\":\""
                << json_escape(to_posix_path(rel)) << "\"}\n";
      return 2;
    }
    std::string write_err;
    if (!write_text_file_all(dest, *content_opt, write_err)) {
      std::cout << "{\"ok\":false,\"error\":\"restore_write_failed\",\"path\":\""
                << json_escape(to_posix_path(rel)) << "\"}\n";
      return 2;
    }
    restored.push_back(to_posix_path(rel));
  }

  std::sort(restored.begin(), restored.end());
  restored.erase(std::unique(restored.begin(), restored.end()), restored.end());

  std::cout << "{\"ok\":true,\"snapshot_id\":\"" << json_escape(snapshot_id)
            << "\",\"restored\":[";
  for (std::size_t i = 0; i < restored.size(); i++) {
    if (i) std::cout << ",";
    std::cout << "\"" << json_escape(restored[i]) << "\"";
  }
  std::cout << "]}\n";
  return 0;
}

static std::optional<std::string> arg_value(int argc, char** argv,
                                           const std::string& key) {
  for (int i = 0; i + 1 < argc; i++) {
    if (argv[i] == key) return std::string(argv[i + 1]);
  }
  return std::nullopt;
}

int main(int argc, char** argv) {
  // 入口：按“子命令”的方式分发（类似 git 的 git status / git log）。
  // 这种设计非常利于未来扩展更多工具能力：只要新增一个 cmd_xxx + 参数解析即可。
  if (argc < 2) {
    print_usage(argv[0]);
    return 2;
  }
  std::string cmd = argv[1];

  if (cmd == "list-files") {
    auto root = arg_value(argc, argv, std::string("--root"));
    if (!root.has_value()) {
      std::cout << "{\"ok\":false,\"error\":\"missing_root\"}\n";
      return 2;
    }
    return cmd_list_files(fs::path(*root));
  }

  if (cmd == "read-file") {
    auto path = arg_value(argc, argv, std::string("--path"));
    if (!path.has_value()) {
      std::cout << "{\"ok\":false,\"error\":\"missing_path\"}\n";
      return 2;
    }
    std::size_t max_bytes = 200000;
    auto mb = arg_value(argc, argv, std::string("--max-bytes"));
    if (mb.has_value()) max_bytes = static_cast<std::size_t>(std::stoull(*mb));
    return cmd_read_file(fs::path(*path), max_bytes);
  }

  if (cmd == "search-text") {
    auto root = arg_value(argc, argv, std::string("--root"));
    auto query = arg_value(argc, argv, std::string("--query"));
    if (!root.has_value() || !query.has_value()) {
      std::cout << "{\"ok\":false,\"error\":\"missing_root_or_query\"}\n";
      return 2;
    }
    int topk = 10;
    auto tk = arg_value(argc, argv, std::string("--topk"));
    if (tk.has_value()) topk = std::stoi(*tk);
    std::size_t max_bytes = 200000;
    auto mb = arg_value(argc, argv, std::string("--max-bytes"));
    if (mb.has_value()) max_bytes = static_cast<std::size_t>(std::stoull(*mb));
    return cmd_search_text(fs::path(*root), *query, topk, max_bytes);
  }

  if (cmd == "apply-edits") {
    auto root = arg_value(argc, argv, std::string("--root"));
    auto edits_json = arg_value(argc, argv, std::string("--edits-json"));
    if (!root.has_value() || !edits_json.has_value()) {
      std::cout << "{\"ok\":false,\"error\":\"missing_root_or_edits_json\"}\n";
      return 2;
    }
    return cmd_apply_edits(fs::path(*root), fs::path(*edits_json));
  }

  if (cmd == "rollback") {
    auto root = arg_value(argc, argv, std::string("--root"));
    auto sid = arg_value(argc, argv, std::string("--snapshot-id"));
    if (!root.has_value() || !sid.has_value()) {
      std::cout << "{\"ok\":false,\"error\":\"missing_root_or_snapshot_id\"}\n";
      return 2;
    }
    return cmd_rollback(fs::path(*root), *sid);
  }

  print_usage(argv[0]);
  return 2;
}
