#!/usr/bin/env python3
"""Assemble design/human/*.md, in lexical order, into a single book-style PDF.

Each document is munged into a chapter of one book:
  - 000.Introduction.md supplies the book's title and subtitle, and its
    sections become chapters of their own.
  - Every other document's "# Title" becomes a chapter that starts on a new page.
  - Prose paragraphs separated by single newlines are split into real paragraphs.
  - Links between documents (like [038.Mail.md](038.Mail.md)) become links to
    the chapter, labeled with the chapter's title.

Requires pandoc and typst; install them with design/install.pandoc.py.

Usage: python3 design/assemble.py [--keep-markdown]
Output: design/the-human-design-document.pdf
"""

import argparse
import datetime
import os
import re
import shutil
import subprocess
import sys
import tempfile

DESIGN = os.path.dirname(os.path.abspath(__file__))
HUMAN = os.path.join(DESIGN, "human")
OUTPUT = os.path.join(DESIGN, "the-human-design-document.pdf")
AUTHOR = "Jeffrey M. Barber"

HEADING = re.compile(r"^(#{1,6})\s*(.*?)\s*#*\s*$")
DOC_LINK = re.compile(r"\[([^\]]*)\]\(((\d{3})\.[^)]*\.md)\)")

# Typst rules injected into the document preamble to give it a book layout.
TYPST_PREAMBLE = r"""
#set page(numbering: "1", number-align: center)
#set par(justify: true)
#show heading.where(level: 1): it => { pagebreak(weak: true); v(2em); it; v(1em) }
#show raw.where(block: true): set text(size: 8pt)
#show raw.where(block: true): block.with(fill: luma(245), inset: 8pt, radius: 3pt, width: 100%)
#show table: set text(size: 9pt)
#set table(inset: 5pt)
#set table.cell(align: left + top)
#show link: set text(fill: rgb("#1f4e8c"))
#show table.cell: set par(justify: false)
#show figure: set block(breakable: true)
#show heading: set block(above: 1.4em, below: 0.8em)
"""


def find_tool(name):
    """Find a tool on PATH, falling back to where install.pandoc.py puts it."""
    found = shutil.which(name)
    if found:
        return found
    local = os.path.expanduser(f"~/.local/bin/{name}")
    if os.access(local, os.X_OK):
        return local
    sys.exit(f"{name} not found; run: python3 design/install.pandoc.py")


def is_structural(line):
    """Lines that must stay attached to their neighbors (tables, lists, quotes)."""
    stripped = line.lstrip()
    return (stripped.startswith("|") or stripped.startswith(">")
            or re.match(r"^([-*+]|\d+\.)\s", stripped) is not None
            or line.startswith((" ", "\t")))


def split_paragraphs(lines):
    """Outside code fences, give every prose line and heading its own block.

    The design docs never hard-wrap prose, so a single newline between two
    lines of prose is always a paragraph break that markdown would swallow.
    """
    out, in_fence = [], False
    for line in lines:
        if line.lstrip().startswith("```"):
            if not in_fence and out and out[-1] != "":
                out.append("")
            in_fence = not in_fence
            out.append(line)
            continue
        if in_fence:
            out.append(line)
            continue
        if line.strip() == "":
            out.append("")
            continue
        previous = out[-1] if out else ""
        joinable = previous != "" and is_structural(previous) and is_structural(line)
        if previous != "" and not joinable:
            out.append("")
        out.append(line)
        if HEADING.match(line):
            out.append("")
    # Collapse runs of blank lines.
    collapsed = []
    for line in out:
        if line == "" and collapsed and collapsed[-1] == "":
            continue
        collapsed.append(line)
    return collapsed


def read_document(path):
    with open(path, encoding="utf-8") as f:
        return [line.rstrip() for line in f.read().replace("\r\n", "\n").split("\n")]


def take_title_block(lines):
    """Remove the intro's '# Title' plus a setext subtitle; return (title, subtitle, rest)."""
    title, subtitle, rest = None, None, list(lines)
    while rest and rest[0].strip() == "":
        rest.pop(0)
    match = HEADING.match(rest[0]) if rest else None
    if match and len(match.group(1)) == 1:
        title = match.group(2)
        rest.pop(0)
        # A line underlined by dashes is a setext heading: treat it as the subtitle.
        if len(rest) >= 2 and rest[0].strip() and re.fullmatch(r"-{3,}|={3,}", rest[1].strip()):
            subtitle = rest[0].strip()
            rest = rest[2:]
    return title, subtitle, rest


def promote_headings(lines):
    out, in_fence = [], False
    for line in lines:
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
        match = None if in_fence else HEADING.match(line)
        if match and len(match.group(1)) > 1:
            line = "#" * (len(match.group(1)) - 1) + " " + match.group(2)
        out.append(line)
    return out


def tag_chapters(lines, number):
    """Give the first chapter heading of a document an id so links can target it."""
    out, tagged, title, in_fence = [], False, None, False
    for line in lines:
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
        match = None if in_fence else HEADING.match(line)
        if match and len(match.group(1)) == 1 and not tagged:
            title = match.group(2)
            line = f"# {title} {{#ch-{number}}}"
            tagged = True
        out.append(line)
    return out, title


def munge(paths):
    """Turn each document into chapter markdown; return (metadata, markdown)."""
    meta = {"title": "Design", "subtitle": None}
    chapters, titles = [], {}
    for index, path in enumerate(paths):
        number = os.path.basename(path)[:3]
        lines = read_document(path)
        if index == 0:
            title, subtitle, lines = take_title_block(lines)
            meta["title"] = title or meta["title"]
            meta["subtitle"] = subtitle
            lines = promote_headings(lines)
        lines = split_paragraphs(lines)
        lines, chapter_title = tag_chapters(lines, number)
        titles[number] = chapter_title or os.path.basename(path)
        chapters.append("\n".join(lines))

    def relink(match):
        text, target, number = match.groups()
        if number not in titles:
            return match.group(0)
        label = titles[number] if text.strip() == target else text
        return f"[{label}](#ch-{number})"

    markdown = "\n\n".join(DOC_LINK.sub(relink, chapter) for chapter in chapters)
    return meta, markdown


def yaml_string(value):
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--keep-markdown", action="store_true",
                        help="also write the combined markdown next to the PDF, for debugging")
    args = parser.parse_args()

    paths = sorted(os.path.join(HUMAN, name) for name in os.listdir(HUMAN) if name.endswith(".md"))
    if not paths:
        sys.exit(f"no markdown files in {HUMAN}")
    pandoc, typst = find_tool("pandoc"), find_tool("typst")

    meta, markdown = munge(paths)
    front = ["---", f"title: {yaml_string(meta['title'])}"]
    if meta["subtitle"]:
        front.append(f"subtitle: {yaml_string(meta['subtitle'])}")
    front += [f"author: {yaml_string(AUTHOR)}",
              f"date: {yaml_string(datetime.date.today().strftime('%B %-d, %Y'))}",
              "header-includes: |",
              "  ```{=typst}"]
    front += ["  " + line for line in TYPST_PREAMBLE.strip().splitlines()]
    front += ["  ```", "---", ""]
    book = "\n".join(front) + markdown + "\n"

    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "book.md")
        with open(source, "w", encoding="utf-8") as f:
            f.write(book)
        if args.keep_markdown:
            shutil.copy(source, os.path.splitext(OUTPUT)[0] + ".md")
        command = [pandoc, source, "--from=markdown-tex_math_dollars-citations", "--to=pdf",
                   f"--pdf-engine={typst}", "--toc", "--toc-depth=1",
                   "--number-sections",
                   "--variable=papersize:us-letter", "--variable=margin.x:1in",
                   "--variable=margin.y:1in", "--variable=fontsize:11pt",
                   f"--output={OUTPUT}"]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write(result.stderr)
            sys.exit(f"pandoc failed with exit code {result.returncode}")
        if result.stderr.strip():
            sys.stderr.write(result.stderr)

    print(f"assembled {len(paths)} documents into {os.path.relpath(OUTPUT)}")


if __name__ == "__main__":
    main()
