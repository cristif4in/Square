/* =====================================================================
 *  Square Browser Engine — square_content.cpp
 *  HTML5 tokenizer + tree builder, CSS parser, selector matching,
 *  cascade / computed styles, serialization.
 * ===================================================================== */
#include "square_content_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <unordered_set>

namespace sqrl {

Document::~Document() {
  if (root) node_free(root);
}

/* ============================ small utils ============================ */
 std::string lc(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}
 std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isspace((unsigned char)s[a])) a++;
  while (b > a && isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}
 bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

/* ============================ character references ============================ */
struct EntEntry { const char* name; const char* utf8; };
static const EntEntry k_entities[] = {
  {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
  {"nbsp", "\xc2\xa0"}, {"copy", "\xc2\xa9"}, {"reg", "\xc2\xae"}, {"trade", "\xe2\x84\xa2"},
  {"hellip", "\xe2\x80\xa6"}, {"mdash", "\xe2\x80\x94"}, {"ndash", "\xe2\x80\x93"},
  {"lsquo", "\xe2\x80\x98"}, {"rsquo", "\xe2\x80\x99"}, {"ldquo", "\xe2\x80\x9c"}, {"rdquo", "\xe2\x80\x9d"},
  {"bull", "\xe2\x80\xa2"}, {"middot", "\xc2\xb7"}, {"deg", "\xc2\xb0"}, {"plusmn", "\xc2\xb1"},
  {"times", "\xc3\x97"}, {"divide", "\xc3\xb7"}, {"laquo", "\xc2\xab"}, {"raquo", "\xc2\xbb"},
  {"iexcl", "\xc2\xa1"}, {"cent", "\xc2\xa2"}, {"pound", "\xc2\xa3"}, {"euro", "\xe2\x82\xac"},
  {"sect", "\xc2\xa7"}, {"para", "\xc2\xb6"}, {"szlig", "\xc3\x9f"}, {"agrave", "\xc3\xa0"},
  {"aacute", "\xc3\xa1"}, {"egrave", "\xc3\xa8"}, {"eacute", "\xc3\xa9"}, {"uuml", "\xc3\xbc"},
  {"ouml", "\xc3\xb6"}, {"auml", "\xc3\xa4"}, {"szlig;", "\xc3\x9f"},
  {"larr", "\xe2\x86\x90"}, {"uarr", "\xe2\x86\x91"}, {"rarr", "\xe2\x86\x92"}, {"darr", "\xe2\x86\x93"},
  {"harr", "\xe2\x86\x94"}, {"minus", "\xe2\x88\x92"}, {"lowast", "\xe2\x88\x97"},
  {"radic", "\xe2\x88\x9a"}, {"infin", "\xe2\x88\x9e"}, {"ne", "\xe2\x89\xa0"}, {"le", "\xe2\x89\xa4"},
  {"ge", "\xe2\x89\xa5"}, {"alpha", "\xce\xb1"}, {"beta", "\xce\xb2"}, {"Omega", "\xce\xa9"},
  {"spades", "\xe2\x99\xa0"}, {"clubs", "\xe2\x99\xa3"}, {"hearts", "\xe2\x99\xa5"}, {"diams", "\xe2\x99\xa6"},
  {"check", "\xe2\x9c\x93"}, {"cross", "\xe2\x9c\x97"}, {"star", "\xe2\x98\x85"},
  {"permil", "\xe2\x80\xb0"}, {"prime", "\xe2\x80\xb2"}, {"Prime", "\xe2\x80\xb3"},
  {"lsaquo", "\xe2\x80\xb9"}, {"rsaquo", "\xe2\x80\xba"}, {"oline", "\xe2\x80\xbe"},
  {"frasl", "\xe2\x81\x84"}, {"euro;", "\xe2\x82\xac"}, {"shy", "\xc2\xad"},
  {"ensp", "\xe2\x80\x82"}, {"emsp", "\xe2\x80\x83"}, {"thinsp", "\xe2\x80\x89"},
  {"zwnj", "\xe2\x80\x8c"}, {"zwj", "\xe2\x80\x8d"}, {"lrm", "\xe2\x80\x8e"}, {"rlm", "\xe2\x80\x8f"},
};
void decode_entities_into(const std::string& in, std::string* out) {
  out->reserve(out->size() + in.size());
  size_t i = 0, n = in.size();
  while (i < n) {
    if (in[i] == '&') {
      size_t e = in.find(';', i);
      if (e != std::string::npos && e - i <= 10) {
        std::string ent = in.substr(i + 1, e - i - 1);
        bool done = false;
        if (!ent.empty() && (ent[0] == '#' || ((ent[0] == 'x' || ent[0] == 'X') && ent[1] == '#'))) {
          long cp;
          if (ent[0] == '#') {
            if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) cp = strtol(ent.c_str() + 2, nullptr, 16);
            else cp = strtol(ent.c_str() + 1, nullptr, 10);
          } else cp = strtol(ent.c_str() + 2, nullptr, 16);
          if (cp > 0 && cp <= 0x10FFFF) {
            if (cp < 0x80) out->push_back((char)cp);
            else if (cp < 0x800) { out->push_back((char)(0xC0 | (cp >> 6))); out->push_back((char)(0x80 | (cp & 63))); }
            else if (cp < 0x10000) { out->push_back((char)(0xE0 | (cp >> 12))); out->push_back((char)(0x80 | ((cp >> 6) & 63))); out->push_back((char)(0x80 | (cp & 63))); }
            else { out->push_back((char)(0xF0 | (cp >> 18))); out->push_back((char)(0x80 | ((cp >> 12) & 63))); out->push_back((char)(0x80 | ((cp >> 6) & 63))); out->push_back((char)(0x80 | (cp & 63))); }
            done = true;
          }
        } else {
          for (auto& en : k_entities) {
            std::string nm = en.name;
            if (!nm.empty() && nm.back() == ';') nm.pop_back();
            if (nm == ent) {
              *out += en.utf8;
              done = true;
              break;
            }
          }
          if (!done) {
            /* entity with trailing semicolon variants like "amp;" handled by loop */
          }
        }
        if (done) { i = e + 1; continue; }
      }
    }
    out->push_back(in[i]);
    i++;
  }
}

/* ============================ HTML tokenizer + tree ============================ */
static const char* k_void_elements[] = {
  "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
  "param", "source", "track", "wbr", nullptr
};
static bool is_void(const std::string& t) {
  for (int i = 0; k_void_elements[i]; i++) if (t == k_void_elements[i]) return true;
  return false;
}
static bool is_raw_text(const std::string& t) {
  return t == "script" || t == "style" || t == "textarea" || t == "title" || t == "xmp";
}
/* tags that implicitly close an open <p> */
static bool closes_p(const std::string& t) {
  static const char* list[] = {
    "address", "article", "aside", "blockquote", "details", "div", "dl", "fieldset",
    "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6",
    "header", "hgroup", "hr", "main", "menu", "nav", "ol", "p", "pre", "section",
    "table", "ul", nullptr
  };
  for (int i = 0; list[i]; i++) if (t == list[i]) return true;
  return false;
}

struct HtmlParser {
  std::string src;
  size_t pos = 0;
  Node* doc;
  std::vector<Node*> open;   /* stack of open elements */
  Node* current() { return open.empty() ? doc : open.back(); }
  Node* html_el = nullptr, *head_el = nullptr, *body_el = nullptr;
  bool in_head = true;       /* before <body> seen */
  std::string pending_raw_tag;

  HtmlParser(const std::string& s) : src(s) {
    doc = new Node();
    doc->type = N_DOCUMENT;
    doc->tag = "";
    open.push_back(doc);
  }

  Node* append(Node* n) {
    Node* p = current();
    n->parent = p;
    p->children.push_back(n);
    return n;
  }

  void ensure_structure() {
    /* no-op: structure finalized in move_stray_into_body */
  }

  static bool head_ish(const std::string& t) {
    return t == "meta" || t == "link" || t == "title" || t == "style" || t == "base" ||
           t == "noscript" || t == "template" || t == "script";
  }

  void move_stray_into_body() {
    if (!html_el) {
      html_el = new Node();
      html_el->type = N_ELEMENT;
      html_el->tag = "html";
      html_el->parent = doc;
      doc->children.push_back(html_el);
    }
    if (!head_el) {
      head_el = new Node();
      head_el->type = N_ELEMENT;
      head_el->tag = "head";
      head_el->parent = html_el;
      html_el->children.insert(html_el->children.begin(), head_el);
    }
    if (!body_el) {
      body_el = new Node();
      body_el->type = N_ELEMENT;
      body_el->tag = "body";
      body_el->parent = html_el;
      html_el->children.push_back(body_el);
    }
    /* distribute stray nodes at doc level and directly under html */
    std::vector<Node*> stray;
    for (Node* c : doc->children) {
      if (c != html_el && c->type != N_COMMENT && c->type != N_DOCTYPE) stray.push_back(c);
    }
    for (Node* c : html_el->children) {
      if (c != head_el && c != body_el && c->type != N_COMMENT) stray.push_back(c);
    }
    for (Node* c : stray) {
      /* detach from wherever it is */
      if (c->parent) {
        auto& sibs = c->parent->children;
        sibs.erase(std::remove(sibs.begin(), sibs.end(), c), sibs.end());
      }
      if (c->type == N_ELEMENT && head_ish(c->tag)) {
        c->parent = head_el;
        head_el->children.push_back(c);
      } else if (c->type == N_ELEMENT || c->type == N_TEXT) {
        c->parent = body_el;
        body_el->children.push_back(c);
      } else {
        c->parent = doc;
        doc->children.push_back(c);
      }
    }
  }

  std::string read_until(const char* pat) {
    size_t e = src.find(pat, pos);
    if (e == std::string::npos) {
      std::string rest = src.substr(pos);
      pos = src.size();
      return rest;
    }
    std::string r = src.substr(pos, e - pos);
    pos = e + strlen(pat);
    return r;
  }

  void parse_attr_string(const std::string& text, Node* n) {
    size_t i = 0, len = text.size();
    while (i < len) {
      while (i < len && isspace((unsigned char)text[i])) i++;
      if (i >= len) break;
      size_t ns = i;
      while (i < len && text[i] != '=' && !isspace((unsigned char)text[i])) i++;
      std::string name = lc(text.substr(ns, i - ns));
      while (i < len && isspace((unsigned char)text[i])) i++;
      std::string val;
      if (i < len && text[i] == '=') {
        i++;
        while (i < len && isspace((unsigned char)text[i])) i++;
        if (i < len && (text[i] == '"' || text[i] == '\'')) {
          char q = text[i++];
          size_t ve = i;
          while (ve < len && text[ve] != q) ve++;
          val = text.substr(i, ve - i);
          i = ve + 1;
        } else {
          size_t ve = i;
          while (ve < len && !isspace((unsigned char)text[ve])) ve++;
          val = text.substr(i, ve - i);
          i = ve;
        }
      }
      if (!name.empty()) {
        std::string decoded_val;
        decode_entities_into(val, &decoded_val);
        n->set_attr(name, decoded_val);
      }
    }
  }

  void close_tag(const std::string& tag) {
    /* find matching open element (case: lowercase) */
    for (int i = (int)open.size() - 1; i >= 1; i--) {
      if (open[i]->tag == tag) {
        /* pop everything above it too (implied ends) */
        open.resize(i);
        if (tag == "head") in_head = false;
        if (tag == "body" || tag == "html") { /* keep going */ }
        return;
      }
    }
    /* unmatched close: ignore */
  }

  void feed_text(const std::string& raw, bool raw_text_mode) {
    if (raw.empty()) return;
    std::string decoded;
    if (!raw_text_mode) decode_entities_into(raw, &decoded);
    else decoded = raw;
    Node* t = new Node();
    t->type = N_TEXT;
    t->data = decoded;
    append(t);
  }

  void start_tag(const std::string& tag, const std::string& attr_text, bool self_closing) {
    std::string t = lc(tag);
    ensure_structure();
    /* implied ends */
    Node* cur = current();
    if (cur->type == N_ELEMENT) {
      if (closes_p(t) && cur->tag == "p") open.pop_back();
      else if (t == "li" && cur->tag == "li") open.pop_back();
      else if ((t == "dt" || t == "dd") && (cur->tag == "dt" || cur->tag == "dd")) open.pop_back();
      else if ((t == "tr") && (cur->tag == "tr" || cur->tag == "td" || cur->tag == "th")) {
        if (cur->tag != "tr") open.pop_back();
        if (current()->tag == "tr") open.pop_back();
      } else if ((t == "td" || t == "th") && (cur->tag == "td" || cur->tag == "th")) open.pop_back();
      else if (t == "option" && cur->tag == "option") open.pop_back();
      else if (t == "tbody" && (cur->tag == "tr" || cur->tag == "td" || cur->tag == "th")) {
        while (current()->tag == "tr" || current()->tag == "td" || current()->tag == "th") open.pop_back();
      }
      /* implied tbody: <tr> directly in table */
      if ((t == "tr" || t == "td" || t == "th") && current()->tag == "table") {
        Node* tb = new Node();
        tb->type = N_ELEMENT;
        tb->tag = "tbody";
        append(tb);
        open.push_back(tb);
      }
    }
    Node* el = new Node();
    el->type = N_ELEMENT;
    el->tag = t;
    parse_attr_string(attr_text, el);
    if (t == "html") {
      if (!html_el) html_el = el;
      in_head = true;
    }
    append(el);
    if (t == "html") { open.push_back(el); return; }
    if (t == "head") { head_el = el; open.push_back(el); return; }
    if (t == "body") { body_el = el; in_head = false; open.push_back(el); return; }
    if (is_void(t)) return; /* br, img, hr, ... are void: never push to open stack */
    else if (self_closing && t != "script" && t != "style" && t != "title" && t != "textarea") return;
    if (is_raw_text(t)) {
      /* read raw content until </tag> */
      std::string close = "</" + t;
      size_t e = src.find(close, pos);
      std::string content;
      if (e == std::string::npos) {
        content = src.substr(pos);
        pos = src.size();
      } else {
        content = src.substr(pos, e - pos);
        pos = e;
        /* consume '>' of closing tag */
        size_t gt = src.find('>', pos);
        if (gt != std::string::npos) pos = gt + 1;
      }
      Node* txt = new Node();
      txt->type = N_TEXT;
      if (t == "title") {
        /* <title> is RCDATA: decode character references */
        std::string decoded;
        decode_entities_into(content, &decoded);
        txt->data = decoded;
      } else {
        txt->data = content;
      }
      el->children.push_back(txt);
      txt->parent = el;
      return;
    }
    open.push_back(el);
  }

  void parse() {
    size_t n = src.size();
    while (pos < n) {
      if (src[pos] == '<') {
        if (src.compare(pos, 4, "<!--") == 0) {
          size_t e = src.find("-->", pos);
          Node* c = new Node();
          c->type = N_COMMENT;
          if (e == std::string::npos) {
            c->data = src.substr(pos + 4);
            pos = n;
          } else {
            c->data = src.substr(pos + 4, e - pos - 4);
            pos = e + 3;
          }
          append(c);
          continue;
        }
        if (src.compare(pos, 2, "<!") == 0) {
          size_t e = src.find('>', pos);
          std::string content = e == std::string::npos ? src.substr(pos) : src.substr(pos, e - pos + 1);
          pos = e == std::string::npos ? n : e + 1;
          if (content.rfind("<!DOCTYPE", 0) == 0 || content.rfind("<!doctype", 0) == 0) {
            Node* d = new Node();
            d->type = N_DOCTYPE;
            d->data = content;
            append(d);
          }
          continue;
        }
        if (pos + 1 < n && src[pos + 1] == '/') {
          size_t e = src.find('>', pos);
          std::string inner = e == std::string::npos ? src.substr(pos + 2) : src.substr(pos + 2, e - pos - 2);
          pos = e == std::string::npos ? n : e + 1;
          std::string tag = lc(trim(inner));
          close_tag(tag);
          continue;
        }
        if (pos + 1 < n && (isalpha((unsigned char)src[pos + 1]))) {
          /* find tag end (respect quotes) */
          size_t e = pos + 1;
          bool in_q = false;
          char q = 0;
          while (e < n) {
            char c = src[e];
            if (in_q) {
              if (c == q) in_q = false;
            } else if (c == '"' || c == '\'') {
              in_q = true;
              q = c;
            } else if (c == '>') break;
            e++;
          }
          std::string inner = e >= n ? src.substr(pos + 1) : src.substr(pos + 1, e - pos - 1);
          pos = e >= n ? n : e + 1;
          bool selfclose = false;
          if (!inner.empty() && inner.back() == '/') {
            selfclose = true;
            inner.pop_back();
          }
          size_t tp = 0;
          while (tp < inner.size() && !isspace((unsigned char)inner[tp])) tp++;
          std::string tag = inner.substr(0, tp);
          std::string attrs = inner.substr(tp);
          start_tag(tag, attrs, selfclose);
          continue;
        }
        /* lone '<': treat as text */
        feed_text("<", false);
        pos++;
        continue;
      }
      /* text run */
      size_t e = src.find('<', pos);
      if (e == std::string::npos) {
        feed_text(src.substr(pos), false);
        pos = n;
      } else {
        feed_text(src.substr(pos, e - pos), false);
        pos = e;
      }
    }
    move_stray_into_body();
  }
};

static void doc_find_title(Node* head, std::string* out) {
  for (Node* c : head->children) {
    if (c->type == N_ELEMENT && c->tag == "title") {
      for (Node* t : c->children)
        if (t->type == N_TEXT) *out = trim(t->data);
    }
  }
}

Document* html_parse_document(const std::string& bytes) {
  /* charset: assume utf8 (meta charset checked lightly) */
  HtmlParser p(bytes);
  p.parse();
  Document* d = new Document();
  d->root = p.doc;
  d->html = p.html_el ? p.html_el : p.doc->children.size() ? p.doc->children[0] : nullptr;
  d->head = p.head_el;
  d->body = p.body_el;
  if (!d->html || d->html->type != N_ELEMENT) {
    /* synthesize html/body */
    Node* h = new Node();
    h->type = N_ELEMENT;
    h->tag = "html";
    h->parent = p.doc;
    Node* b = new Node();
    b->type = N_ELEMENT;
    b->tag = "body";
    b->parent = h;
    for (Node* c : p.doc->children) {
      if (c->type == N_ELEMENT && c->tag == "html") { h = c; continue; }
    }
    p.doc->children.clear();
    p.doc->children.push_back(h);
    h->children.clear();
    h->children.push_back(b);
    d->html = h;
    d->body = b;
  }
  if (!d->body) {
    /* find or create body */
    for (Node* c : d->html->children)
      if (c->type == N_ELEMENT && c->tag == "body") { d->body = c; break; }
    if (!d->body) {
      Node* b = new Node();
      b->type = N_ELEMENT;
      b->tag = "body";
      b->parent = d->html;
      d->html->children.push_back(b);
      d->body = b;
    }
  }
  if (!d->head) {
    for (Node* c : d->html->children)
      if (c->type == N_ELEMENT && c->tag == "head") { d->head = c; break; }
  }
  if (d->head) doc_find_title(d->head, &d->title);
  /* mark svg subtrees */
  std::function<void(Node*)> mark = [&](Node* n) {
    for (Node* c : n->children) {
      if (c->type == N_ELEMENT && c->tag == "svg") c->in_svg = true;
      if (c->in_svg) {
        c->ns = "svg";
        for (auto& cc : c->children) { cc->in_svg = true; cc->ns = "svg"; std::function<void(Node*)> m2 = [&](Node* x) {
          for (Node* k : x->children) { k->in_svg = true; k->ns = "svg"; m2(k); }
        }; m2(c); }
      }
      mark(c);
    }
  };
  mark(d->root);
  return d;
}

std::vector<Node*> html_parse_fragment(const std::string& html) {
  HtmlParser p(html);
  /* parse into a temp doc then steal children */
  p.parse();
  std::vector<Node*> out;
  for (Node* c : p.body_el ? p.body_el->children : p.doc->children) {
    out.push_back(c);
  }
  /* clear parents so caller can adopt */
  for (Node* c : out) {
    c->parent = nullptr;
  }
  /* detach adopted nodes from the temp tree, then free the scaffold
   * (html/head/body shells + head content that was never adopted) */
  if (p.body_el) p.body_el->children.clear();
  else p.doc->children.clear();
  node_free(p.doc);
  /* recursively keep ns marking for svg fragments */
  std::function<void(Node*)> mark = [&](Node* n) {
    for (Node* c : n->children) {
      if (c->type == N_ELEMENT && c->tag == "svg") {
        std::function<void(Node*)> m2 = [&](Node* x) {
          x->in_svg = true;
          x->ns = "svg";
          for (Node* k : x->children) m2(k);
        };
        m2(c);
      }
      mark(c);
    }
  };
  for (Node* c : out) mark(c);
  return out;
}

void node_free(Node* n) {
  if (!n) return;
  for (Node* c : n->children) node_free(c);
  n->children.clear();
  if (n->cs) delete n->cs;
  if (n->pseudo[0]) delete n->pseudo[0];
  if (n->pseudo[1]) delete n->pseudo[1];
  if (n->box) free_box_tree(n->box);   /* frees kids/out_of_flow recursively */
  delete n;
}

/* ============================ serialization ============================ */
static void escape_html(const std::string& s, std::string* out) {
  for (char c : s) {
    switch (c) {
      case '&': *out += "&amp;"; break;
      case '<': *out += "&lt;"; break;
      case '>': *out += "&gt;"; break;
      default: out->push_back(c);
    }
  }
}
static void serialize_node(const Node* n, std::string* out) {
  switch (n->type) {
    case N_TEXT: escape_html(n->data, out); return;
    case N_COMMENT: *out += "<!--" + n->data + "-->"; return;
    case N_DOCTYPE: *out += n->data; return;
    default: break;
  }
  *out += "<" + n->tag;
  for (auto& a : n->attrs) {
    *out += " " + a.name + "=\"";
    escape_html(a.value, out);
    *out += "\"";
  }
  if (is_void(n->tag)) { *out += ">"; return; }
  *out += ">";
  for (const Node* c : n->children) serialize_node(c, out);
  *out += "</" + n->tag + ">";
}
std::string serialize_html(const Node* n) {
  std::string out;
  serialize_node(n, &out);
  return out;
}

} /* namespace sqrl */
