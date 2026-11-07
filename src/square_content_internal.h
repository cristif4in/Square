/* =====================================================================
 *  Square Browser Engine — internal header shared by the content module:
 *    square_content.cpp   HTML tokenizer + tree builder + serialization
 *    square_css.cpp       CSS tokenizer, selectors, stylesheet parser
 *    square_style.cpp     CSS values, declaration application, cascade
 *  Not installed publicly — engine-internal only.
 * ===================================================================== */
#pragma once

#include "square_internal.h"

#include <string>
#include <vector>

namespace sqrl {

/* ---- small string utils (defined in square_content.cpp) ---- */
std::string lc(std::string s);
std::string trim(const std::string& s);
bool starts_with(const std::string& s, const char* p);

/* ---- character references (defined in square_content.cpp) ---- */
void decode_entities_into(const std::string& in, std::string* out);

/* ---- declaration-block parser (defined in square_css.cpp) ---- */
void parse_declarations(const std::string& body, std::vector<Declaration>* out);

/* ---- CSS tokenizer cursor (shared by stylesheet + value parsing) ---- */
struct CssCursor {
  const std::string& s;
  size_t i = 0;
  explicit CssCursor(const std::string& str) : s(str) {}
  void ws() {
    while (i < s.size()) {
      if (isspace((unsigned char)s[i])) i++;
      else if (s.compare(i, 2, "/*") == 0) {
        size_t e = s.find("*/", i + 2);
        i = e == std::string::npos ? s.size() : e + 2;
      } else break;
    }
  }
  bool eof() { ws(); return i >= s.size(); }
  char peek() { ws(); return i < s.size() ? s[i] : 0; }
  void skip_until_char(char c) {
    while (i < s.size()) {
      if (s.compare(i, 2, "/*") == 0) {
        size_t e = s.find("*/", i + 2);
        i = e == std::string::npos ? s.size() : e + 2;
        continue;
      }
      if (s[i] == '"' || s[i] == '\'') {
        char q = s[i++];
        while (i < s.size() && s[i] != q) {
          if (s[i] == '\\') i++;
          i++;
        }
        i++;
        continue;
      }
      if (s[i] == c) return;
      if (s[i] == '{') { skip_block(); continue; }
      i++;
    }
  }
  /* skip a balanced { ... } block starting at '{' */
  void skip_block() {
    if (i >= s.size() || s[i] != '{') return;
    int depth = 0;
    while (i < s.size()) {
      char c = s[i];
      if (c == '"' || c == '\'') {
        char q = c;
        i++;
        while (i < s.size() && s[i] != q) {
          if (s[i] == '\\') i++;
          i++;
        }
        i++;
        continue;
      }
      if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
        size_t e = s.find("*/", i + 2);
        i = e == std::string::npos ? s.size() : e + 2;
        continue;
      }
      if (c == '{') depth++;
      if (c == '}') {
        depth--;
        i++;
        if (depth == 0) return;
        continue;
      }
      i++;
    }
  }
  std::string read_until_chars(const char* chars) {
    ws();
    size_t st = i;
    while (i < s.size()) {
      char c = s[i];
      if (c == '"' || c == '\'') {
        char q = c;
        i++;
        while (i < s.size() && s[i] != q) {
          if (s[i] == '\\') i++;
          i++;
        }
        i++;
        continue;
      }
      if (strchr(chars, c)) break;
      i++;
    }
    return s.substr(st, i - st);
  }
  std::string read_url() {
    /* cursor at "url(" */
    i += 4;
    ws();
    std::string out;
    if (i < s.size() && (s[i] == '"' || s[i] == '\'')) {
      char q = s[i++];
      size_t st = i;
      while (i < s.size() && s[i] != q) i++;
      out = s.substr(st, i - st);
      if (i < s.size()) i++;
    } else {
      size_t st = i;
      while (i < s.size() && s[i] != ')') i++;
      out = trim(s.substr(st, i - st));
    }
    if (i < s.size() && s[i] == ')') i++;
    return out;
  }
};

} /* namespace sqrl */
