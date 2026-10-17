# HTML → plain text in open-source mail clients — algorithms & recommended C++ design

Scope: what each project actually does (from source), then one concrete, dependency-free
algorithm fit for Forma's `applications/mail/mime.cpp:htmlText()`.

---

## 1. Thunderbird (C++ libmime + Gecko `nsPlainTextSerializer`)

**Where the conversion happens.** The HTML-as-plaintext part is `mimeInlineTextHTMLAsPlaintext`
(`mimethpl.cpp`). libmime feeds it decoded bytes line by line; it buffers the whole part and at
EOF calls `HTML2Plaintext()` with flags `OutputFormatted|OutputWrap|OutputFormatFlowed|OutputLFLineBreak|OutputNoScriptContent|OutputNoFramesContent|OutputBodyOnly`, wrap at **80** columns.
`HTML2Plaintext` (mimemoz2.cpp) is just `nsIParserUtils::ConvertToPlainText` → Gecko's
`nsPlainTextSerializer` (mozilla-central `dom/serializers/nsPlainTextSerializer.cpp`). The
libmime C++ side contains almost no conversion logic — all the rules are in the serializer:

| Concern | Rule in `nsPlainTextSerializer.cpp` |
|---|---|
| Block model | Event-driven walk. Each block end schedules a *pending* break: `mFloatingLines` = number of blank lines to insert *before the next text* (0 = just end line, 1 = one blank line), `mLineBreakDue`. `EnsureVerticalSpace(n)` actually emits the breaks (never trailing). |
| `p`, headings open | `p` → EnsureVerticalSpace(1); `h1..h6` → EnsureVerticalSpace(2); `blockquote` (non-cite) → 1 + indent +4 |
| Block close | `div`, `tr`, `li`, `dt`: end line, 0 blank lines; `p`, `pre`, `ul/ol`(outer), `dl`, `blockquote`: end line + 1 blank line; generic `display:block` → 1 blank line |
| Lists | `ul/ol` open/close: blank line outside lists, none when nested; indent `kIndentSizeList = 4` per level. `li` gets header `"* "` for `ul` (depth cycles `* o + #`, index `(ulCount-1)%4`), `"N. "` for `ol` (`<ol start>` and `<li value>` honoured). |
| `dl/dt/dd` | `dd` indents +4 |
| Blockquote/cite | `<blockquote type="cite">` → `>` quote depth instead of indentation (RFC-3676 style `>`), others → indent |
| Cells | `tr` end line; `td/th` insert a literal **TAB** between cells (bypassing whitespace collapsing) |
| `br` | EnsureVerticalSpace(0) — line break, no blank line (skips Thunderbird's own "padding for empty last line" br) |
| `hr` | line of `-` as wide as wrap column |
| Links | on `</a>` append ` <href>` verbatim (no "same text" check) |
| Images | `alt` verbatim; else `title` as ` [title] `; else nothing; `alt=""` → nothing |
| Whitespace | Source `\n` in text → single space if `!mInWhitespace`; runs of ` \t\n\r` collapse to one space (`mInWhitespace`); `&nbsp;` → plain space in flowed/formatted output. Preformatted zones (computed `white-space: pre*/pre-wrap`, or `<body style="white-space:…">` heuristics) keep raw whitespace. |
| Hidden content | `<script>/<style>` ignored only if computed `display:none` (DOM walk); `noscript`/`iframe`/`noframes` skipped unless flags `OutputNoScriptContent`/`OutputNoFramesContent` (set for mail, so skipped); `<select>` children suppressed; ruby `rt/rp` skipped. There's no *textual* `display:none` scan — the DOM serializer has computed style; libmime's path doesn't. |
| Wrapping | Reflows text to 80 cols with word wrap + format=flowed soft breaks (space stuffing, signature `-- ` protection). |
| Trailing blanks | EndLine trims trailing spaces; no trailing blank lines emitted. |

Thunderbird sanitization of the HTML for *display* (not conversion): `HTMLSanitize()` in
mimemoz2.cpp → `nsIParserUtils::Sanitize` with `SanitizerCidEmbedsOnly|SanitizerDropForms`
(+ prefs `mailnews.display.html_sanitizer.drop_non_css_presentation`, `.drop_media`).

Takeaway: TB's quality comes from (a) **pending-break** bookkeeping so blank lines never land at
the top/bottom, (b) treating every "block" as "end line now, blank line before next text",
(c) layout-aware DOM + computed style.

## 2. GNOME Geary (Vala)

Two layers:
* **Display** is WebKit (`WebKitWebView`); Geary sanitizes the message with **html-cleaner**
  (Gumbo-based): rewrites CSS, strips classes/ids, drops non-whitelisted style props, keeping only
  presentational ones, to stop tracking/marketing CSS from breaking layout. Not a text converter.
* **Plain-text conversion**: `src/engine/util/util-html.vala: html_to_text()` — libxml2 HTML parser
  (`RECOVER|NOERROR|NOWARNING|NOBLANKS|NONET|COMPACT`) then a recursive walk:
  - `breaking_elements` → append `"\n"` after the element: address, blockquote, br, caption,
    center, div, dt, embed, form, h1–h6, hr, iframe, li, map, menu, noscript, object, p, pre, tr.
  - `spacing_elements` → append `" "` after: dt, dd, img, td, th.
  - `alt_text_elements` = {img}: append `alt` text before recursing.
  - `ignored_elements` (children skipped, but their own tail break still runs): base, link, meta,
    head, script, style, template.
  - text nodes appended verbatim; **no** entity decoding (libxml does it) and **no** list bullets
    — `li` is merely a line break. The "very approximate" comment is literal.
  Search strings (snippet bodies) use this; it's cheap, not pretty.

## 3. K-9 Mail / Thunderbird for Android (Java/Kotlin)

Two generations:
* **Modern (≥ 2018)**: `legacy/core/src/main/java/com/fsck/k9/message/html/HtmlToPlainText.kt`
  (a Jsoup `NodeVisitor`, adapted from jsoup's `HtmlToPlainText` example). This is the good one:
  - `TextNode` → append text. `append()` drops a `" "` when output is empty or last char is
    space/newline — i.e. whitespace collapse happens because Jsoup gives text nodes with
    whitespace as distinct `" "` nodes (pretty-printing breaks become single spaces, else dropped).
  - `li` head: `startNewLine()` then `"* "`; `li` tail: `"\n"`.
  - `br` tail: `"\n"`.
  - every Jsoup *block* element (`Element.isBlock`: p, div, table, tr, h1–6, ul/ol, blockquote…)
    head: `startNewLine()`; tail: if the element has text, `addEmptyLine()` (one blank line,
    deduped: never two in a row).
  - `a[href]`: collects its text; on close, if `absUrl(href)` is non-empty and differs from the
    displayed text, append `" <" + href + ">"`.
  - final trim of trailing newlines.
  In `HtmlConverter.kt`: result has `U+FFFC` (object replacement char, from spans) replaced by
  space and `nbsp` (U+00A0) replaced by plain space — exactly the "tofu box" fix.
  The WebView display path uses `HtmlSanitizer` (jsoup Safelist, `library/html-cleaner`) to strip
  script/iframe/forms/objects; conversion path uses no sanitizer (text output is inert anyway).
* **Classic (2013–2018)**: `HtmlConverter.htmlToText` = `android.text.Html.fromHtml` + a
  `TagHandler`: `style/script/title/!` (comments) content deleted via annotation spans; `<hr>` →
  48 underscores; then `U+FFFC→space`, `NBSP→space`. Its `htmlToSpanned` list handler: `ul` →
  CRLF around the list, `li` → `"\t•  "` and CRLF. (The "regex converter" reputation comes from
  `textToHtml`, not from HTML→text; also `<a>` URLs were never emitted in that generation.)

Takeaway: **`* ` bullets, block → one blank line deduped, links as `text <url>` only when url ≠
text, NBSP/FFFC → space** is the core of the best-regarded converter, and it fits in ~150 lines.

## 4. KDE PIM / KMime (C++)

Verified against the repo (invent.kde.org/pim/kmime, `src/`): KMime has **no HTML→text** support.
It handles MIME structure/headers, encodings, charsets (`codecs.cpp`), and a `Content::guessCharset`
/ OSS-Fuzz corpus; `content.cpp` only mentions RFC 2045's `text/plain` default. Preview text in
PIM apps (Akonadi) is taken from the decoded part; HTML rendering is a WebEngine view in KMail
with its own sanitizer. → nothing to borrow beyond "don't attempt layout, just decode".

## 5. mutt / neomutt (C)

No built-in HTML→text. `handler.c` `alternative_handler()` picks the `multipart/alternative`
part by: `alternative_order` list → first autoviewable type → `text/plain` (TXTPLAIN 2) >
`text/enriched` (TXTRICHED 3) > `text/html` (TXTHTML 1). A chosen `text/html` part goes through
`mutt_body_handler` → autoview check (`mailcap` with `copiousoutput`) → `rfc1524_mailcap_lookup`;
with no mailcap entry it's dumped raw to the pager. Canonical config from the manual:

```
auto_view text/html
# and in mailcap:
text/html;      lynx -dump -width=78 -nolist %s; nametemplate=%s.html; copiousoutput
```

(`%s` is the temp file; `-nolist` drops mutt's link footnotes; `-width` matters since mutt's pager
shows what the program prints). `mime_lookup` only maps mislabelled parts by **filename
extension** (`attach.c: mutt_check_lookup_list`), e.g. `mime_lookup application/octet-stream`.
`mailcap` in neomutt is literally the name of the project — `html2text`/`w3m -dump`/`lynx -dump`
handlers are the de-facto built-ins. Lesson: **fallback ordering matters** and "HTML as the last
resort among text/*" matches both mutt and TB.

## 6. Python

* **html2text** (Alir3z4, `html2text/__init__.py`): outputs *Markdown*: `p` blank line,
  `br` two spaces + newline, `ul li` `"  * "`, `ol` `"  N. "`, `blockquote` `"> "`, `img` →
  `![alt](src)`, `a` → `[text](href)`. Useful bits: `UNIFIABLE` entity map (`rsquo→'`,
  `ndash→-`, `mdash→--`, `rarr→->`, `nbsp→space`, `copy→(C)`, `oelig→oe` …) — the same idea we
  want, and `utils.py: control_character_replacements` — Windows-1252 reinterpretation of
  C1 controls `0x80–0x9F` (Euro, smart quotes, dashes, bullet, TM…), from the WHATWG spec.
  A second pass then collapses >2 blank lines and re-wraps text.
* **alot** (notmuch client): *does not* convert HTML itself. `db/utils.py: get_body_part()` uses
  Python `email.get_body(preferencelist)` with `('plain','html')` (config `prefer_plaintext`) or
  `('html','plain')`; `render_part` → `mailcap` handler if the type has one, else falls back to
  text/plain, and for an un-handled text/html part shows a "no handler / failed" placeholder.
  User side: `envelope_html2txt` command (e.g. `pandoc -f html -t plain` / `html2text`).
  `pazz/alot` `helper.py: string_sanitize()` is the *rendering* guard: it strips every
  non-printable char except `\n`/`\t` (Unicode categories Cc, Cn, Co are removed), expands tabs
  to 8 columns, and only lets through SGR ANSI sequences — the direct answer to "no tofu / no
  escape injection in the terminal".

## 7. Cross-cutting "garbage that looks like a box" inventory

| Codepoint / construct | Where it comes from | What clients do |
|---|---|---|
| U+00A0 NBSP | `&nbsp;` used as layout space (cell padding, indentation) | convert to space for display (K-9 explicit) |
| U+00AD soft hyphen | copy-paste from Word | drop |
| U+200B ZWSP, U+200C/D ZWNJ/ZWJ, U+2060, U+FEFF | `<wbr>`, Word/EOM markers | drop |
| U+FFFC | Android object spans | K-9 → space |
| C0/C1 controls (0x00–0x1F except \t\n, 0x7F–0x9F) | bad quoted-printable, broken charsets | alot removes all `Cc`; html2text re-maps 0x80–0x9F from Windows-1252 (`â€™` double-encoding never produces these, but `&rsquo;` mis-decoded can) |
| U+200E/200F/202A–202E | bidi marks | harmless in most UIs, drop |
| U+E000–F8FF PUA | icon fonts (Font Awesome `<i class="fa">`), Word Wingdings bullets | render garbage/tofu → drop |

---

# 8. Recommended algorithm (C++20, no dependencies)

Target: marketing HTML (table layout, Outlook `mso` comments, inline styles, web-font icons).
Input is the **already charset-decoded UTF-8** body (Forma already does `utf8(data, charset)`).

### Design
A single left-to-right tokenizer (no DOM, no tree) with the following state:

```text
struct Conv {
    std::string out;                  // final text
    int blanks    = 2;                // consecutive '\n' already in out (2 = start/blank line)
    bool pendingBreak = false;        // a break must be emitted before next visible text
    int  pendingBlanks = 1;           // 0 = end line only, 1 = one blank line
    bool inWS = true;                 // last emitted char is space or newline
    bool pre  = false;                // inside <pre>/white-space:pre* → keep whitespace
    bool header = false;              // body has white-space:pre-wrap (Thunderbird) → keep '\n', collapse spaces
    int  skip = 0;                    // >0: suppressed tag (script/style/head/iframe/title/noscript/select/map)
    bool inTable = false, rowOpen = false; bool cellOpen/… ;
    std::vector<std::pair<char,int>> lists;   // ('u'|'o', nextNumber)
    std::vector<int> headings;               // open h1..h6
    struct Link { std::string href, text; } link; bool inLink = false;
    int quote = 0;                    // <blockquote> depth
    std::string cellBuf; bool inCell = false; // multi-line cells buffered for column layout
};
```

### 8.1 Pipeline (top level)

```text
convert(html):
    html = preClean(html)                 // 8.2: comments, mso, display:none
    tokenize(html)                        // 8.3: text | <tag attrs> | </tag> | void elements
    finalize()                            // 8.7: trim, limit blank runs
    return out
```

### 8.2 Pre-clean (regex-ish scans, linear, done before tokenizing)

```text
preClean(s):
  1. Remove ALL comments:  "<!--" … "-->"
       (this alone removes every Outlook mso comment, both "
        <!--[if !mso]><!-->…<!--<![endif]-->  visible-for-non-Outlook wraps
        and <!--[if mso]>…<![endif]--> hidden ones, plus [if gte mso 9] xml islands,
        <o:p>…</o:p> are NOT comments — drop <o:p>…</o:p> too, see 8.3.)
       Unclosed comment → cut to EOF.
     NOTE: after deleting comments the visible-for-modern wrappers disappear as a pair and
     their content survives; the mso-only wrappers' content vanishes together with the tags.
  2. Remove blocks whose START TAG has a hidden/zero-size style:
       <(div|span|p|td|tr|table|center|font|a|li|ul|ol|noscript)\b[^>]*\bstyle="[^"]*(display:\s*none|visibility:\s*hidden|mso-hide:\s*all|font-size:\s*0(px)?\b|max-height:\s*0|width:\s*0(px)?;?[^"]*height:\s*0|opacity:\s*0\b)[^"]*"[^>]*>
       → delete from match to its matching close tag (count same-name opens/closes; stop at EOF).
     Also delete elements with hidden=""/hidden, and `class` tokens preheader/hide.
     (Outlook preheader spans use display:none — they must not become the preview line.)
  3. Drop <meta>, <link>, <title>…</title>, <base>, <xml>…</xml>, <o:…>…</o:…>, <v:…>…</v:…>
     (VML shapes render as broken-image boxes).
```

All three passes run on the *raw* string before entity decoding, so quoted attributes can be
matched naively as long as quotes are respected (`[^"]*` between `="…"`; also handle `'…'`).

### 8.3 Tokenizer

```text
for i in s:
  if s[i] != '<':   emitText(s[i])                              ; continue
  if s starts "<!--": (already removed) skip to "-->"
  if s starts "<!": skip to '>'
  tag = parse: '<' ['/'] name [attrs] ('/>' | '>')     // respect quoted attribute values
  name = ascii_lower(name);  if void(name) → leaf(name,attrs) else if close → endTag(name) else startTag(name,attrs)
```

### 8.4 Text output primitives (the heart)

```text
emitText(t):
    if skip > 0: return
    if inLink: link.text += t
    for each codepoint c in decodeEntities(t):                   // 8.6
        sanitize(c)                                              // 8.6 → may map to ' ' or drop
        if pre:                 putRaw(c)                        // keep \n and spaces
        elif header and c == '\n':  lineBreak(0)                 // TB pre-wrap mail: keep hard newlines
        elif isSpace(c) or c == '\n' or c == '\t': inWS = true   // defer: nothing emitted yet
        else:                   putRaw(c)

putRaw(c):                               // visibility tracking
    if inWS and !pre:                    // collapse: we owe exactly one space
        if c != ' ': out += ' '           // (leading space suppressed because blanks>=1 occupies the line)
    inWS = (c==' '); flushPending()      // pending break happens before this char
    if !pendingBreak && c != ' ' && blanks>=2 => nothing special
    out += c

flushPending():                          // "break before the next text" – never trailing
    // A single space owed by inWS must not produce a space at line start.
    while pendingBreak and blanks < 1 + pendingBlanks: out += '\n'; blanks++
    inWS = true; pendingBreak = false

lineBreak(n):    // n = blank lines wanted *after* this line (0 or 1)
    trimTrailingSpaces(out); inWS = true
    if blanks >= 1 + n: return           // already at/below desired, nothing pending
    pendingBreak = true; pendingBlanks = n
```

Why "pending": `<p>a</p><p>b</p>` must give `a\n\nb` and not `a\n\n` + `\n\nb` (three blank
lines) and never leave trailing blank lines. This is Thunderbird's `mLineBreakDue/mFloatingLines`
and jsoup's `addEmptyLine()` in 20 lines of C++.

### 8.5 Tag rules

```text
BLOCK(endLine only):      address article aside center dd div dl dt figcaption figure footer
                          form h1..h6 header li main nav p section table caption thead tbody tfoot tr ul ol
BLOCK(blank line=1):      p, div, h1..h6, blockquote, table, ul, ol, dl, header/footer/section
BLOCK(blank line=0):      tr, li, dt, dd, td? (cells handled separately)
SKIP(content dropped):    script style head title iframe object embed noscript template svg math
                          select textarea button (keep <button> text? no → drop), map area
INLINE(drop, no output):  span b i u strong em small big font sup sub code kbd mark abbr a
                          (only attributes matter: style on span can be display:none – pre-cleaned)

startTag(name, attrs):
  if skip: if name == skipTag return (nested same tag); else return
  switch name:
    script|style|head|title|iframe|object|embed|noscript|template|svg|math|select|textarea: skip = name; return
    body:   header = attrs.style contains "white-space:pre"     // TB heuristic for pre-wrap mail
    br:     lineBreak(0)
    hr:     lineBreak(0); flushPending(); out += string(72,'-'); lineBreak(1)  // TB width = wrap col; K-9 uses 48…'_
    p:      flushPending(); lineBreak(1)
    div|center|section|article|header|footer|aside|figure|figcaption|form|main|address:
            flushPending(); lineBreak(headings.empty()? 1 : 0)  // inside heading: no blank lines
    h1..h6: lineBreak(2 → clamp rule: open heading = blank line before) : lineBreak(1); headings.push(level)
    blockquote: lineBreak(1); quote++                     // rendered as indent in 8.7 ("> " optional)
    pre:    lineBreak(1); pre=true
    ul|ol:  if lists.empty() lineBreak(1); lists.push({ol?'o':'u', start attr or 1})
    li:     lineBreak(0); flushPending(); out += indentForLists() + bullet(); lineBreak(0)
            // bullet: ul → "* " at depth1, nested depth d → "*" repeated? Keep it simple:
            //   "* " for ul, "N. " (N = ++counter) for ol, indented by 4 per outer level:
            //   "    * ", "    3. " etc.
    dl:     lineBreak(1); dt: lineBreak(0); dd: lineBreak(0), +4 indent
    table:  lineBreak(1); inTable=true
    tr:     lineBreak(0); ensure cells of previous row flushed; rowOpen=true
    td|th:  if inTable: inCell=true; startCell()          // 8.8
    a:      if attrs.href non-empty and usable (see 8.9): inLink=true; link={href, ""}
    img:    if inTable && inCell: append to cell text; else emitImg(attrs)   // 8.9
    wbr|area|col|colgroup|meta|link|base|input|param|source|track: nothing
    o:p|o:*|v:*|w:*: nothing (after pre-clean they are gone anyway)

endTag(name):
  if skip: if name == skip: skip=0; return
  switch name:
    p|div|center|…: flushPending(); lineBreak(1)
    h1..h6: headings.pop(); lineBreak(1) (or 2 after h1/h2? keep 1)
    blockquote: lineBreak(1); quote--
    pre:    pre=false; lineBreak(1)
    ul|ol:  flushPending(); lineBreak(lists.size()>1 ? 0 : 1); lists.pop()
    li|tr:  lineBreak(0)
    td|th:  if inCell: endCell()
    table:  inTable=false; lineBreak(1)
    a:      if inLink: endLink()                          // 8.9
    br:     (already a leaf)
```

Content-suppression correctness notes:
* `skip` only drops *text and nested tags*, exactly like TB's `mIgnoreAboveIndex` and jsoup's
  `Element.isBlock` handling in K-9; a `<br>` inside a skipped subtree is also dropped.
* The **conditional-comment** problem is solved entirely by 8.2 step 1; keep a cheap safety net in
  `startTag`: if name starts with `[` or `!` (a leftover `<![if mso]>` or `<![endif]` that the
  comment scanner saw as a bogus comment), drop it.

### 8.6 Entities and character sanitizing

```text
decodeEntities(text):       // one pass, per codepoint
    '&' followed by  [A-Za-z][A-Za-z0-9]{1,31} ';'  → named[LOWER(name)]   (prefix match: "&ampx" → no match; "&nbsp" without ';' accepted)
    '&#' digits ';' (1..7) / '&#x' hexdigit+ ';'    → codepoint (WHATWG numeric rules below)
    else literal '&'

NAMED table:  amp lt gt quot apos → basic five
              nbsp → ' '            (U+00A0)
              ensp emsp thinsp → ' ' ; zwsp zwnj zwj lrm rlm → "" ; shy → ""
              copy → "(C)"  (optional; TB does not, keep (C) only if you prefer ASCII)   – or "©"
              rsquo lsquo → '\''  rdquo ldquo → '"'  ndash → '-'  mdash → "-"  hellip → "..."
              rarr → "->"  larr → "<-"  middot → "*"  bull → "*"  times → "x"
              euro → "€" (or "EUR");  #8203/#65279 numeric forms → ""
              laquo/raquo/excl/quest/deg/plusmn/micro → keep as UTF-8 literal
```

Numeric reference decode (follow WHATWG parser):
```text
cp = value
if cp == 0 or cp in D800..DFFF or (cp in 0x80..0x9F and not decimal-with-leading-zero) and cp in win1252 → remap cp=win1252[cp]
if cp > 0x10FFFF or surrogates or noncharacter (FDD0..FDEF, *FFFE/FFFF) → U+FFFD… or drop (prefer drop for mail)
else UTF-8 encode
```
Minimum practical version: accept only `cp in {9, A, D, 20..D7FF, E000..10FFFF minus FDD0..FFFF and *FFFE/FFFF}`;
everything else → space (never emit raw surrogates/overlong sequences; a decoder bug there is the
classic source of `�`).

`sanitize(cp)` — the "no tofu" table (run for every decoded codepoint, also for literal text):

```text
switch:
  U+00A0                          → ' '
  U+00AD (soft hyphen)            → drop
  U+200B 200C 200D 2060 FEFF      → drop
  U+200E 200F 202A..202E          → drop (bidi marks)
  U+FFFC FFFD                     → ' '   (K-9's object char / decoder failure)
  U+E000..U+F8FF                  → drop (icon-font PUA glyphs: Font Awesome/Wingdings → boxes)
  U+FFFE U+FFFF U+FDD0..FDEF      → drop
  U+0000..U+001F except \t \n \r  → drop   (alot removes Cc wholesale)
  U+007F..U+009F                  → remap via Windows-1252 if known (0x92→’), else drop
  U+2028 2029 (LS/PS)             → '\n' / '\n' or space
  U+00B7 middle dot, U+2022       → keep (fine fonts) — or map 2022 → '*'
  U+2610..U+2612, U+2714, … checkboxes/emoji symbols → keep if font covers them, else optional ASCII map
  else                            → cp
```
For a UI whose font fallback is good (Forma uses fontconfig fallbacks), keep real punctuation
(‘ ’ “ ” – — … • €) as UTF-8. Only *invisible* and *private-use* codepoints must go.

### 8.7 Finalize

```text
finalize():
    trim trailing spaces/newlines from out
    collapse any run of >2 '\n' to exactly 2          // belt-and-braces, headers/odd markup can still slip through
    normalise "\r\n"→"\n", lone "\r"→"\n"
    replace "\t" introduced by table cells only if not keeping a table grid
    optionally: strip lines that are empty-but-indented (from lists across blocks)
    drop a leading "    * " / "1. " artefact if list started at blank state? (rare; skip)
```

### 8.8 Tables (marketing mail is 95 % tables)

Goal: never lose text, never produce a blank line per cell.

* `captureNested=true` check: `if the table has a single logical row/column count <= 1, or a
  cell contains a <table>, treat as layout table`.
* **Layout tables** (heuristic: `role="presentation"`, `cellpadding=0 border=0`, or ≥ 2 nesting
  levels, or `attrs` have width in %/px on table): ignore all table structure; just `lineBreak(1)`
  at `</table>`. Because 8.4 emits a **space between cells** (set `inWS=false` after endCell) and a
  line break at `</tr>`, a two-column footer "© 2025 Acme | Unsubscribe" comes out on one line.
* **Data tables** (`<table>` with `<th>` or with a `<caption>` or the first row all `<th>`):
  buffer each cell: `cellBuf` collects cell text as *one collapsed line* (`collapse(cell, " ")`),
  letting `<br>` inside a cell become `" / "`. At `</tr>` measure max width (display width, count
  CJK/emoji as 2) and pad columns with spaces to `maxw + 3`, header row followed by a
  `'-' * totalWidth` rule (TB writes `-`×wrapcol for `hr`; same idea per row). Wide rows
  (> 100 cols) fall back to one cell per line.
* `colspan/rowspan`: just emit the text once in the first row it appears; don't build a grid.
* `width=100%` spacer cells and image-only cells yield empty text → skipped (no blank lines).

### 8.9 Links and images

```text
emitImg(attrs):
    alt = attrs.alt; if alt == "" return                     // explicit empty alt = decorative
    if alt:         out += alt                              // TB: alt verbatim; K-9/Geary: alt
    elif attrs.title: out += " [" + title + "]"              // TB
    elif !inLink and attrs.src is an http(s) URL and attrs.src not data::
                        out += " [image: " + src + "]"       // optional; drop for cleaner text

endLink():
    t = collapse(link.text, " ")
    if t.empty():            out += "<" + href + ">"        // image link / icon link → URL is the only content
    elif absLike(t) or t == href: /* nothing */             // "https://x" linking to itself
    elif href starts "mailto:" and t == href[7:]: nothing
    elif realUrl(href):      out += " <" + href + ">"       // TB/K-9 behaviour
    else                    /* internal #anchor, javascript:, empty */ nothing
```
Link text is passed through `emitText` normally (so `<a>Sig<b>n</b> <i>off</i></a>` renders
correctly); if capturing into a separate buffer complicates the collapse logic, a simpler variant
is: emit link text inline and only remember `href` + `linkStart` offset in `out`; at `</a>`
compare `trim(out.substr(linkStart))` with `href`. That is what this converter should do — it
avoids a second text pipeline.

### 8.10 Worked behaviour (what the output should look like)

```html
<!-- marketing snippet -->
<div style="display:none;max-height:0">Preheader text</div>
<table role="presentation"><tr>
  <td><img src="logo.png" alt="Acme"></td>
  <td><h1 style="margin:0">Spring&nbsp;sale</h1></td>
</tr><tr><td colspan=2>
  <p>Up to <b>50%</b> off&nbsp;&mdash; today&nbsp;only.</p>
  <ul><li>Shirts</li><li>Shoes&nbsp;&rsquo;25</li></ul>
  <p><a href="https://acme.test/s">Shop&nbsp;now</a> or <a href="https://acme.test/u"><img alt=""></a></p>
</td></tr></table>
```
→
```
Acme

Spring sale

Up to 50% off - today only.

* Shirts
* Shoes '25

Shop now <https://acme.test/s> or <https://acme.test/u>
```
(Preheader skipped by 8.2 step 2; the logo alt stays because it has non-empty alt; layout table
adds no blank lines between cells; `&nbsp;` never breaks the collapse.)

### 8.11 Suggested constants for a first iteration

| Setting | Value | Source |
|---|---|---|
| blank lines between blocks | 1 (so ≤ 2 `\n` total) | jsoup/K-9 `addEmptyLine` |
| list indent | 0 + `"* "` / `"2. "` at top level, +4 per nesting | K-9 `"* "`; TB 4/level |
| unordered bullet cycle (nested) | `* o + #` | TB |
| link suffix | `text <url>` only if url ≠ text and is absolute | K-9, TB |
| image | `alt`; `title` in `[…]` when no alt | TB |
| `hr` | 48 × `_` (K-9) or 72 × `-` (TB) | either |
| quoted-printable soft breaks | already handled upstream | — |
| wrapping | none (do it in the text view) | K-9 does none; TB wraps at 80 |

### 8.12 Testing checklist (golden files)

1. `multipart/alternative` with a real marketing mail and its plain part → HTML path must be used
   only when plain part is "markdown-ish"/low quality (Forma already has a heuristic).
2. `<!--[if mso]>` block and `<!--[if !mso]><!-->` wrapper.
3. `display:none` preheader.
4. Nested 3-level tables with `width="100%"`, `role="presentation"`.
5. `&nbsp;&nbsp;&nbsp;` indentation padding strips to single spaces.
6. soft hyphen between lines after `<br>` in a link: `<a href=x>Click<wbr>here</a>`.
7. `<img alt=""><img alt="">` (spacer images) produce nothing and no blank lines.
8. `<style>` with `@media` and `mso` CSS (braces, `>` in selectors) — must be skipped entirely.
9. Numeric entities: `&#160; &#xAD; &#8203; &#65279; &#150;` (C1 → en dash via cp1252).
10. 40 consecutive `<br>`s → output has at most one blank line.
11. Chinese body with `&#20013;` etc. (Forma's tests already cover 中文).
12. `</p>` runs without open `<p>` (browsers auto-close) → no blank-line explosion.

---

## Sources

* Thunderbird libmime: `mailnews/mime/src/mimethpl.cpp` (github.com/mozilla/releases-comm-central),
  `mimemoz2.cpp` (`HTML2Plaintext`, `HTMLSanitize`), `mimetext.cpp` (text/plain-as-HTML wrapper).
  https://github.com/mozilla/releases-comm-central/blob/master/mailnews/mime/src/mimethpl.cpp
* Gecko serializer (all conversion rules): https://github.com/mozilla/gecko-dev/blob/master/dom/serializers/nsPlainTextSerializer.cpp
* Geary: https://gitlab.gnome.org/GNOME/geary/-/blob/main/src/engine/util/util-html.vala (`html_to_text`)
* K-9/TbA modern: https://github.com/thunderbird/thunderbird-android/blob/main/legacy/core/src/main/java/com/fsck/k9/message/html/HtmlToPlainText.kt ,
  `HtmlConverter.kt` (FFFC/NBSP replacement) same directory; sanitizer `library/html-cleaner/.../HtmlSanitizer.kt`.
* K-9 classic: tag-handler converter at commit `300076c5` (2018) `app/core/src/main/java/com/fsck/k9/message/html/HtmlConverter.java`.
* KMime: https://invent.kde.org/pim/kmime (`src/` — no HTML conversion).
* mutt: https://github.com/muttmua/mutt/blob/master/handler.c (`alternative_handler`), `attach.c` (`mutt_check_lookup_list`),
  `doc/manual.xml.head` (mailcap `copiousoutput`, lynx/w3m examples).
* html2text: https://github.com/Alir3z4/html2text/blob/master/html2text/config.py (`UNIFIABLE`), `html2text/utils.py` (`control_character_replacements`).
* alot: https://github.com/pazz/alot/blob/master/alot/helper.py (`string_sanitize`), `alot/db/utils.py` (`get_body_part`, `extract_body_part`),
  `alot/commands/envelope.py` (`html2txt` with `pandoc`), `alot/defaults/alot.rc.spec` (`envelope_html2txt`).
