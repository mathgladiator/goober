#include "goober/render/mustache.h"

#include <charconv>
#include <memory>
#include <vector>

#include "goober/base/encoding.h"

namespace goober::render {
namespace {

struct Node {
  enum class Kind { kText, kVar, kSection, kInverted, kPartial };
  Kind kind = Kind::kText;
  std::string text;  // the literal text, or the tag's name
  bool raw = false;  // {{{x}}} or {{& x}}
  std::string indent;  // a standalone partial's indentation
  std::vector<Node> children;
};

bool is_blank(char c) { return c == ' ' || c == '\t'; }

std::string_view trim(std::string_view s) {
  while (!s.empty() && (is_blank(s.front()) || s.front() == '\n' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (is_blank(s.back()) || s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

// Parses a template into a tree, applying the spec's standalone-line rule: a section,
// inverted, closing, comment, or partial tag alone on its line takes the line with it.
class Parser {
 public:
  explicit Parser(std::string_view t) : t_(t) {}

  static constexpr size_t kMaxDepth = 64;
  static constexpr size_t kMaxBytes = 1 << 20;

  bool parse(std::vector<Node>* out, std::string* error) {
    if (t_.size() > kMaxBytes) return fail(error, "a template is at most " + std::to_string(kMaxBytes) + " bytes");
    std::vector<std::vector<Node>*> stack{out};
    std::vector<std::string> open;
    size_t pos = 0;
    while (pos < t_.size()) {
      size_t at = t_.find("{{", pos);
      if (at == std::string_view::npos) {
        add_text(*stack.back(), t_.substr(pos));
        break;
      }
      bool triple = t_.substr(at, 3) == "{{{";
      size_t inner = at + (triple ? 3 : 2);
      size_t close = t_.find(triple ? "}}}" : "}}", inner);
      if (close == std::string_view::npos) return fail(error, "unclosed tag at byte " + std::to_string(at));
      size_t end = close + (triple ? 3 : 2);
      std::string_view body = trim(t_.substr(inner, close - inner));
      char sigil = triple ? '{' : (body.empty() ? '\0' : body.front());
      bool standalone_kind = sigil == '#' || sigil == '^' || sigil == '/' || sigil == '!' || sigil == '>';
      if (sigil == '=') return fail(error, "set-delimiter tags aren't supported");
      std::string name(trim(standalone_kind || sigil == '&' ? body.substr(1) : body));
      if (sigil != '!' && name.empty()) return fail(error, "empty tag at byte " + std::to_string(at));

      // Standalone: only blanks between the line's start and the tag, and between the tag and
      // the line's end. Text before the tag on its line that holds an earlier tag isn't blank.
      // Only the text since the last tag is searched, so parsing stays linear in the template.
      size_t nl = t_.substr(pos, at - pos).rfind('\n');
      size_t line_start = nl != std::string_view::npos       ? pos + nl + 1
                          : pos == 0 || t_[pos - 1] == '\n' ? pos
                                                            : std::string_view::npos;
      bool before_blank = line_start != std::string_view::npos;
      for (size_t i = line_start; before_blank && i < at; ++i) before_blank = is_blank(t_[i]);
      size_t after = end;
      while (after < t_.size() && is_blank(t_[after])) ++after;
      bool after_blank = after == t_.size() || t_[after] == '\n' ||
                         (t_[after] == '\r' && after + 1 < t_.size() && t_[after + 1] == '\n');
      bool standalone = standalone_kind && before_blank && after_blank;

      std::string indent;
      if (standalone) {
        indent = std::string(t_.substr(line_start, at - line_start));
        add_text(*stack.back(), t_.substr(pos, line_start - pos));
        end = after == t_.size() ? after : (t_[after] == '\r' ? after + 2 : after + 1);
      } else {
        add_text(*stack.back(), t_.substr(pos, at - pos));
      }
      pos = end;

      switch (sigil) {
        case '!':
          break;
        case '#':
        case '^': {
          Node n;
          n.kind = sigil == '#' ? Node::Kind::kSection : Node::Kind::kInverted;
          n.text = name;
          // Rendering recurses once per open section: bounded, so no template can exhaust the stack.
          if (open.size() >= kMaxDepth) return fail(error, "sections nest deeper than " + std::to_string(kMaxDepth));
          stack.back()->push_back(std::move(n));
          stack.push_back(&stack.back()->back().children);
          open.push_back(name);
          break;
        }
        case '/':
          if (open.empty() || open.back() != name) {
            return fail(error, "{{/" + name + "}} closes " + (open.empty() ? "nothing" : "{{#" + open.back() + "}}"));
          }
          open.pop_back();
          stack.pop_back();
          break;
        case '>': {
          Node n;
          n.kind = Node::Kind::kPartial;
          n.text = name;
          n.indent = standalone ? indent : "";
          stack.back()->push_back(std::move(n));
          break;
        }
        default: {
          Node n;
          n.kind = Node::Kind::kVar;
          n.text = name;
          n.raw = sigil == '{' || sigil == '&';
          stack.back()->push_back(std::move(n));
        }
      }
    }
    if (!open.empty()) return fail(error, "{{#" + open.back() + "}} is never closed");
    return true;
  }

 private:
  static void add_text(std::vector<Node>& nodes, std::string_view s) {
    if (s.empty()) return;
    if (!nodes.empty() && nodes.back().kind == Node::Kind::kText) {
      nodes.back().text += s;
      return;
    }
    Node n;
    n.text = std::string(s);
    nodes.push_back(std::move(n));
  }
  static bool fail(std::string* error, std::string why) {
    if (error) *error = std::move(why);
    return false;
  }
  std::string_view t_;
};

std::string to_text(const bj::value& v) {
  switch (v.kind()) {
    case bj::kind::string: return std::string(v.get_string());
    case bj::kind::int64: return std::to_string(v.get_int64());
    case bj::kind::uint64: return std::to_string(v.get_uint64());
    case bj::kind::double_: {
      char buf[32];
      auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v.get_double());
      return ec == std::errc() ? std::string(buf, end) : "";
    }
    case bj::kind::bool_: return v.get_bool() ? "true" : "false";
    default: return "";
  }
}

bool truthy(const bj::value* v) {
  if (!v || v->is_null()) return false;
  if (v->is_bool()) return v->get_bool();
  if (v->is_array()) return !v->get_array().empty();
  if (v->is_string()) return !v->get_string().empty();
  return true;
}

class Renderer {
 public:
  Renderer(const TemplateOptions& o, std::string* error) : o_(o), error_(error) {}

  bool render(const std::vector<Node>& nodes, std::vector<const bj::value*>& stack, std::string& out,
              const std::string& indent, int depth) {
    for (const auto& n : nodes) {
      if (out.size() > o_.max_output_bytes) {
        if (error_) *error_ = "the page is larger than " + std::to_string(o_.max_output_bytes) + " bytes";
        return false;
      }
      switch (n.kind) {
        case Node::Kind::kText:
          append(out, n.text, indent);
          break;
        case Node::Kind::kVar: {
          size_t frame = 0;
          const bj::value* v = lookup(stack, n.text, &frame);
          if (!v) break;
          std::string s = to_text(*v);
          bool trusted = n.raw && frame == 0 && o_.trusted_html.count(n.text);
          append(out, trusted ? s : base::html_escape(s), "");
          break;
        }
        case Node::Kind::kSection: {
          const bj::value* v = lookup(stack, n.text, nullptr);
          if (!truthy(v)) break;
          if (v->is_array()) {
            for (const auto& item : v->get_array()) {
              stack.push_back(&item);
              bool ok = render(n.children, stack, out, indent, depth);
              stack.pop_back();
              if (!ok) return false;
            }
          } else {
            stack.push_back(v);
            bool ok = render(n.children, stack, out, indent, depth);
            stack.pop_back();
            if (!ok) return false;
          }
          break;
        }
        case Node::Kind::kInverted:
          if (!truthy(lookup(stack, n.text, nullptr)) && !render(n.children, stack, out, indent, depth)) return false;
          break;
        case Node::Kind::kPartial: {
          if (!o_.partial) break;
          if (depth >= o_.max_partial_depth) {
            if (error_) *error_ = "partials nest deeper than " + std::to_string(o_.max_partial_depth) + " at " + n.text;
            return false;
          }
          if (++partials_ > o_.max_partials) {
            if (error_) *error_ = "more than " + std::to_string(o_.max_partials) + " partials in one page";
            return false;
          }
          auto text = o_.partial(n.text);
          if (!text) break;
          std::vector<Node> tree;
          std::string err;
          if (!Parser(*text).parse(&tree, &err)) {
            if (error_) *error_ = "partial " + n.text + ": " + err;
            return false;
          }
          if (!n.indent.empty()) out += n.indent;
          if (!render(tree, stack, out, indent + n.indent, depth + 1)) return false;
          break;
        }
      }
    }
    return true;
  }

 private:
  // Appends text, indenting every line after a newline (for standalone partials).
  static void append(std::string& out, std::string_view s, const std::string& indent) {
    if (indent.empty()) {
      out += s;
      return;
    }
    for (size_t i = 0; i < s.size(); ++i) {
      out += s[i];
      if (s[i] == '\n' && i + 1 < s.size()) out += indent;
    }
  }

  static const bj::value* lookup(const std::vector<const bj::value*>& stack, std::string_view name, size_t* frame) {
    if (name == ".") {
      if (frame) *frame = stack.size() - 1;
      return stack.back();
    }
    std::string_view first = name.substr(0, name.find('.'));
    const bj::value* v = nullptr;
    for (size_t i = stack.size(); i-- > 0;) {
      if (stack[i]->is_object()) {
        if (auto* f = stack[i]->get_object().if_contains(first)) {
          v = f;
          if (frame) *frame = i;
          break;
        }
      }
    }
    if (!v) return nullptr;
    size_t pos = first.size();
    while (pos < name.size()) {
      size_t next = name.find('.', pos + 1);
      std::string_view part = name.substr(pos + 1, next == std::string_view::npos ? std::string_view::npos : next - pos - 1);
      if (!v->is_object()) return nullptr;
      v = v->get_object().if_contains(part);
      if (!v) return nullptr;
      pos = next == std::string_view::npos ? name.size() : next;
    }
    return v;
  }

  const TemplateOptions& o_;
  std::string* error_;
  int partials_ = 0;
};

}  // namespace

bool check_mustache(std::string_view tmpl, std::string* error) {
  std::vector<Node> tree;
  return Parser(tmpl).parse(&tree, error);
}

std::optional<std::string> render_mustache(std::string_view tmpl, const bj::value& data,
                                           const TemplateOptions& options, std::string* error) {
  std::vector<Node> tree;
  if (!Parser(tmpl).parse(&tree, error)) return std::nullopt;
  std::vector<const bj::value*> stack{&data};
  std::string out;
  if (!Renderer(options, error).render(tree, stack, out, "", 0)) return std::nullopt;
  return out;
}

}  // namespace goober::render
