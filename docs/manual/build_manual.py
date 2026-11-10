#!/usr/bin/env python3
"""Builds the PDF manuals for Chloaelis and Chloaeilius (black pages, white text).

The body comes from the READMEs (so the manual and the README say the same thing), the parameter
reference from the plug-in itself (the host tests dump it with MANUAL_DUMP=<folder>), and the
pictures from docs/. Printed to PDF by Chrome (headless).

    python3 docs/manual/build_manual.py chloaelis   <dump folder> <out.pdf>
    python3 docs/manual/build_manual.py chloaeilius <dump folder> <out.pdf>
"""
import html, json, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

def esc(s): return html.escape(s, quote=False)

# ------------------------------------------------------------------------------------------------
# a small Markdown reader: headings, paragraphs, nested lists, tables, code, quotes, images, links
def inline(t, base):
    t = esc(t)
    t = re.sub(r"`([^`]+)`", r"<code>\1</code>", t)
    def img(m):
        src = m.group(2)
        if not re.match(r"https?:", src): src = "file://" + os.path.normpath(os.path.join(base, src))
        return "<img class='inl' src='%s' alt='%s'>" % (src, m.group(1))
    t = re.sub(r"!\[([^\]]*)\]\(([^)]+)\)", img, t)
    def link(m):
        href = m.group(2)
        if href.startswith("#") or not re.match(r"https?:", href): return m.group(1)   # (in the PDF: just the words)
        return "<a href='%s'>%s</a>" % (href, m.group(1))
    t = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", link, t)
    t = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", t)
    t = re.sub(r"(?<![\w*])\*([^*]+)\*(?![\w*])", r"<i>\1</i>", t)
    return t

def markdown(md, base, skip_titles=()):
    lines = md.split("\n")
    out, i = [], 0
    def is_list(l): return re.match(r"^(\s*)([-*]|\d+\.)\s+", l)
    while i < len(lines):
        l = lines[i]
        if l.strip().startswith("<p align") or l.strip().startswith("<h1") or l.strip() == "---":
            # (the README's centred HTML header: the cover replaces it)
            if l.strip().startswith("<p align") and "</p>" not in l:
                while i < len(lines) and "</p>" not in lines[i]: i += 1
            i += 1; continue
        if l.startswith("```"):
            j = i + 1; code = []
            while j < len(lines) and not lines[j].startswith("```"): code.append(lines[j]); j += 1
            out.append("<pre>%s</pre>" % esc("\n".join(code))); i = j + 1; continue
        m = re.match(r"^(#{1,4})\s+(.*)", l)
        if m:
            lvl = len(m.group(1)); title = m.group(2).strip()
            if title in skip_titles:
                # skip this section up to the next heading of the same or a higher level
                i += 1
                while i < len(lines):
                    m2 = re.match(r"^(#{1,4})\s+", lines[i])
                    if m2 and len(m2.group(1)) <= lvl: break
                    i += 1
                continue
            out.append("<h%d>%s</h%d>" % (lvl, inline(title, base), lvl)); i += 1; continue
        if l.startswith(">"):
            q = []
            while i < len(lines) and lines[i].startswith(">"): q.append(lines[i][1:].strip()); i += 1
            out.append("<blockquote>%s</blockquote>" % "".join("<p>%s</p>" % inline(x, base) for x in " \n".join(q).split("\n\n") if x.strip()))
            continue
        if l.startswith("|"):
            rows = []
            while i < len(lines) and lines[i].startswith("|"): rows.append(lines[i]); i += 1
            cells = [[c.strip() for c in r.strip().strip("|").split("|")] for r in rows]
            head, body = cells[0], [r for r in cells[1:] if not re.match(r"^:?-+:?$", r[0] or "-") or any(c and not re.match(r"^:?-+:?$", c) for c in r)]
            body = [r for r in cells[2:]] if len(cells) > 1 and all(re.match(r"^:?-*:?$", c) for c in cells[1]) else cells[1:]
            h = "".join("<th>%s</th>" % inline(c, base) for c in head) if any(head) else ""
            b = "".join("<tr>%s</tr>" % "".join("<td>%s</td>" % inline(c, base) for c in r) for r in body)
            out.append("<table>%s%s</table>" % ("<tr>%s</tr>" % h if h else "", b)); continue
        if is_list(l):
            # nested lists by indentation
            html_parts, stack = [], []
            while i < len(lines) and (is_list(lines[i]) or (lines[i].startswith("  ") and lines[i].strip())):
                m = is_list(lines[i])
                if m:
                    ind = len(m.group(1)); tag = "ol" if m.group(2)[0].isdigit() else "ul"
                    text = lines[i][m.end():]
                    while stack and stack[-1][0] > ind: html_parts.append("</li></%s>" % stack.pop()[1])
                    if not stack or stack[-1][0] < ind: html_parts.append("<%s>" % tag); stack.append((ind, tag))
                    else: html_parts.append("</li>")
                    html_parts.append("<li>" + inline(text, base))
                else:
                    html_parts.append(" " + inline(lines[i].strip(), base))
                i += 1
            while stack: html_parts.append("</li></%s>" % stack.pop()[1])
            out.append("".join(html_parts)); continue
        if not l.strip(): i += 1; continue
        p = []
        while i < len(lines) and lines[i].strip() and not re.match(r"^(#|```|\||>)", lines[i]) and not is_list(lines[i]):
            p.append(lines[i].strip()); i += 1
        txt = " ".join(p)
        if re.fullmatch(r"!\[[^\]]*\]\([^)]+\)", txt):
            out.append("<figure>%s</figure>" % inline(txt, base).replace("class='inl'", "class='fig'"))
        else:
            out.append("<p>%s</p>" % inline(txt, base))
    return "\n".join(out)

def section(md, title):
    """One '## title' section of a README (up to the next '## ')."""
    m = re.search(r"^## %s\s*$" % re.escape(title), md, re.M)
    if not m: return ""
    rest = md[m.end():]
    n = re.search(r"^## ", rest, re.M)
    return md[m.start(): m.end() + (n.start() if n else len(rest))]

# ------------------------------------------------------------------------------------------------
UNITS = {"inatt": " dB", "out": " dB"}
def tidy(pid, text):
    t = text
    if pid.endswith("_cutoff"):
        t = re.sub(r"^([\d.]+)k$", r"\1 kHz", t); t = re.sub(r"^(\d+(?:\.\d+)?)$", r"\1 Hz", t)
    for k, u in UNITS.items():
        if pid.endswith("_" + k) and not t.endswith("dB"): t = t + u if t != "-inf" else "silent"
    return t

def param_table(ps):
    rows = []
    for p in ps:
        if p["type"] == "choice": rng = esc(p.get("choices", ""))
        elif p["type"] == "switch": rng = "off / on"
        else: rng = esc(tidy(p["id"], p["min"])) + " to " + esc(tidy(p["id"], p["max"]))
        rows.append("<tr><td>%s</td><td class='id'>%s</td><td>%s</td><td>%s</td></tr>"
                    % (esc(p["name"]), esc(p["id"]), rng, esc(tidy(p["id"], p["default"]))))
    return "<table class='params'><tr><th>Name in your DAW</th><th>ID</th><th>Range</th><th>Default</th></tr>%s</table>" % "".join(rows)

def params_section(params, spatial):
    left = [p for p in params if p["id"].startswith("l_")]
    right = [p for p in params if p["id"].startswith("r_")]
    shared = [p for p in params if p not in left and p not in right]
    s = ["<h2 id='params'>Parameter reference</h2><p>Every control your DAW can automate, by the name it shows there. "
         "The two sides have the same controls, so the right side's table follows the left's. Defaults are what a fresh instance starts with (reset brings them back).</p>"]
    s.append("<h3>Shared (the middle)</h3>" + param_table(shared))
    s.append("<h3>Left side</h3>" + param_table(left))
    s.append("<h3>Right side</h3>" + param_table(right))
    if spatial:
        s.append("<p class='note'>Distance is the radius of the bubble you're in. Ambisonic Order sets how many channels go out (16 at 3rd order, 36 at 5th, 64 at 7th). Listen only does anything in the desktop app; the plug-ins always send the Ambisonics. Space View changes the picture, never the sound.</p>")
    return "".join(s)

def trouble(spatial):
    items = [
        ("macOS says it can't be opened, or the DAW doesn't list it",
         "The builds aren't notarised. Run <code>xattr -dr com.apple.quarantine</code> on the app and the plug-ins (see Installing), then rescan in your DAW."),
        ("There's no sound and nothing on the input", "Turn up σ (source level) to play a built-in recording through it, and pick which with ∞ / ≈. In the desktop app, check Options for the audio device."),
        ("It keeps sounding after I stop", "Regen past about 90 % sings on its own, and master feedback (Φ) keeps the two sides feeding each other. Turn them down, or reset with ↺."),
        ("A new instance doesn't start from the defaults", "It remembers its last state, so a new instance starts how the last one was closed. A saved project still opens as you saved it. ↺ resets everything (the tape's wear is kept)."),
    ]
    if spatial:
        items.insert(0, ("I only hear two channels, or nothing moves round me",
                         "In a DAW the track needs as many channels as the order switch says (16 at 3rd order, 36 at 5th, 64 at 7th) and a decoder after Chloaeilius at the same order (ACN, SN3D), such as the IEM AllRADecoder or BinauralDecoder. In the desktop app, set Listen to Headphones or Speakers."))
    return "<h2 id='trouble'>Problems and answers</h2>" + "".join("<h3>%s</h3><p>%s</p>" % (esc(a), b) for a, b in items)

CSS = """
@page { size: A4; margin: 16mm 15mm 18mm 15mm; background: #000; }
@font-face { font-family: 'Nunito'; src: url('file://%(font)s'); }
html, body { background: #000; -webkit-print-color-adjust: exact; print-color-adjust: exact; }
body { font-family: 'Avenir Next', 'Helvetica Neue', Arial, sans-serif; font-size: 10.2pt; line-height: 1.45; color: #fff; }
h1, h2, h3, h4 { font-family: 'Nunito', 'Avenir Next', sans-serif; }
h2 { font-size: 20pt; color: %(accent)s; margin: 0 0 6pt; page-break-before: always; letter-spacing: 0.5pt; }
h2.keep { page-break-before: auto; }
h3 { font-size: 12pt; margin: 12pt 0 3pt; color: %(accent2)s; }
h4 { font-size: 10.5pt; margin: 10pt 0 2pt; color: %(accent2)s; }
p { margin: 0 0 6.5pt; }
a { color: %(accent)s; text-decoration: none; }
code, pre { font-family: Menlo, Consolas, monospace; font-size: 8.4pt; color: #fff; }
pre { background: #141418; border: 1px solid #2c2c32; border-radius: 6px; padding: 6pt 8pt; white-space: pre-wrap; }
blockquote { margin: 6pt 0; padding: 4pt 10pt; border-left: 3px solid %(accent)s; background: #0e0e12; color: #e6e6ea; }
ul, ol { margin: 2pt 0 7pt; padding-left: 16pt; }
li { margin-bottom: 2pt; }
figure { margin: 8pt 0 10pt; page-break-inside: avoid; }
img.fig { width: 100%%; border-radius: 6px; border: 1px solid #2c2c32; }
img.inl { width: 72mm; border-radius: 5px; border: 1px solid #2c2c32; }
table { border-collapse: collapse; width: 100%%; margin: 4pt 0 10pt; }
tr { page-break-inside: avoid; }
td, th { text-align: left; vertical-align: top; padding: 3pt 5pt; border-bottom: 1px solid #26262c; }
th { font-size: 8.6pt; color: #a9a9b2; font-weight: 600; }
table.params td { font-size: 8.6pt; }
table.params td.id { font-family: Menlo, monospace; font-size: 7.8pt; color: #8f8f99; }
.cover { height: 255mm; display: flex; flex-direction: column; align-items: center; justify-content: center; text-align: center; }
.cover img.icon { width: 140px; border-radius: 30px; }
.cover h1 { font-family: 'Nunito', sans-serif; font-size: 40pt; margin: 16pt 0 4pt; color: #fff; letter-spacing: 2pt; }
.cover .tag { font-size: 12pt; max-width: 135mm; color: #e6e6ea; }
.cover .ver { margin-top: 12pt; font-size: 10.5pt; color: #a9a9b2; }
.cover img.shot { width: 155mm; margin-top: 18pt; border-radius: 10px; border: 1px solid #2c2c32; }
.toc a { display: block; margin: 3pt 0; color: #fff; }
.note { font-size: 9pt; color: #a9a9b2; }
"""

def build(which, dump, out_pdf):
    spatial = which == "chloaeilius"
    main_md = open(os.path.join(REPO, "README.md")).read()
    params = json.load(open(os.path.join(dump, "params.json")))
    accent, accent2 = ("#7fd4ff", "#c4ecff") if not spatial else ("#b49cff", "#ddd3ff")
    css = CSS % dict(accent=accent, accent2=accent2, font=os.path.join(REPO, "Resources/fonts/NunitoBold.ttf"))
    docs = os.path.join(REPO, "docs")
    if not spatial:
        name, version = "Chloaelis", "1.0.0"
        tag = "A dual-channel filter, tube and delay instrument with a Lorenz attractor, a flock of boids and two field recordings built in."
        shot = os.path.join(docs, "screenshot.png")
        about = ("<p>This manual covers Chloaelis 1.0.0, the plug-in (AU, VST3, LV2) and the desktop app. It follows the README, "
                 "with the full key to the symbols, a reference for every parameter your DAW can automate, and answers to common problems.</p>")
        body = markdown(main_md, REPO, skip_titles=("Contents",))
        # (the README's opening note about Chloaeilius stays: it says where the spatial version is)
    else:
        name, version = "Chloaeilius", "0.2.0"
        tag = "The spatial audio version of Chloaelis: an Ambisonics encoder (1st to 7th order), its echoes flying round you on the flock and the Lorenz attractor."
        shot = os.path.join(REPO, "Chloaeilius/docs/views/chloaeilius_sphere_globe.png")
        sp_md = open(os.path.join(REPO, "Chloaeilius/README.md")).read()
        about = ("<p>This manual covers Chloaeilius 0.2.0, the plug-in (AU, VST3, LV2) and the desktop app. Chloaeilius is the same instrument as Chloaelis, "
                 "so after the spatial chapters it carries Chloaelis's own: what it is, the layout, the symbols and how to play it. "
                 "Then a reference for every parameter, and answers to common problems.</p>")
        body = markdown(sp_md, os.path.join(REPO, "Chloaeilius"))
        body += "<h2>What it is (as in Chloaelis)</h2>" + markdown(section(main_md, "What it is").split("\n", 1)[1], REPO)
        body += "<h2>How to use it (as in Chloaelis)</h2>" + markdown(section(main_md, "How to use it").split("\n", 1)[1], REPO)
    body = re.sub(r"<h2>", "<h2>", body)
    icon = os.path.join(docs, "icon.png")
    parts = [f"""<div class='cover'><img class='icon' src='file://{icon}'><h1>{name}</h1><div class='tag'>{esc(tag)}</div>
<div class='ver'>Manual for version {version}</div><img class='shot' src='file://{shot}'></div>""",
             "<h2>About this manual</h2>" + about, body, params_section(params, spatial), trouble(spatial)]
    doc = "<!doctype html><html><head><meta charset='utf-8'><title>%s manual</title><style>%s</style></head><body>%s</body></html>" % (name, css, "\n".join(parts))
    tmp = tempfile.mkdtemp(prefix="chlmanual_", dir=os.environ.get("MANUAL_TMP"))
    page = os.path.join(tmp, "manual.html")
    open(page, "w").write(doc)
    if os.path.exists(out_pdf): os.remove(out_pdf)
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--no-pdf-header-footer", "--allow-file-access-from-files",
                    "--user-data-dir=" + os.path.join(tmp, "chrome"), "--virtual-time-budget=20000",
                    "--print-to-pdf=" + os.path.abspath(out_pdf), "file://" + page], capture_output=True)
    if not os.path.exists(out_pdf): sys.exit("no PDF written")
    print("wrote", out_pdf, "%.1f MB" % (os.path.getsize(out_pdf) / 1e6))

if __name__ == "__main__":
    build(sys.argv[1], sys.argv[2], sys.argv[3])
