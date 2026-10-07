# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: UPL-1.0 AND BSD-3-Clause

"""Render the public guide allowlist with Pandoc and the shared thc site shell."""

import html
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import posixpath
import re
import shutil
import subprocess
import sys
from urllib.parse import quote, unquote, urlsplit, urlunsplit


ROOT = Path(__file__).resolve().parents[1]
SITE = ROOT / "build/site"
REPO = "https://github.com/ekmett/jam-vm"
PAGES = (
    ("docs/site/index.md", "home.html", "Home"),
    ("docs/build.md", "guides/build.html", "Build and run"),
    ("docs/native-image.md", "guides/native-image.html", "Native Image"),
    ("docs/thc-integration.md", "guides/thc-integration.html", "Integrating thc"),
    ("docs/weak-pointers.md", "guides/weak-pointers.html", "Weak pointers"),
    ("docs/architecture.md", "guides/architecture.html", "Heap architecture"),
    ("docs/hotspot-integration.md", "guides/hotspot-integration.html", "HotSpot integration"),
    ("docs/status.md", "guides/status.html", "Supported configurations"),
    ("docs/documentation.md", "guides/documentation.html", "Documentation"),
    ("LICENSE.md", "license.html", "License"),
)
ASSETS = ("site.css", "site.js", "theme.js", "LICENSE-thc.txt")
DESTINATIONS = {source: page for source, page, _ in PAGES}
DESTINATIONS.update({f"docs/site/{name}": f"assets/{name}" for name in ASSETS})


def git(*args):
    result = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def revision():
    """Return a checked commit, or an explicitly labelled local preview."""
    requested = os.environ.get("DOCS_REVISION")
    head = git("rev-parse", "--verify", "HEAD")
    if requested is not None:
        if not re.fullmatch(r"[0-9a-f]{40}", requested) or requested != head:
            raise ValueError("DOCS_REVISION must be a full lowercase commit ID matching HEAD")
        return requested, False
    if head and git("status", "--porcelain") == "":
        return head, False
    return "main", True


def relative(target, page):
    return posixpath.relpath(target, posixpath.dirname(page) or ".")


def source_url(path, commit, directory=False):
    kind = "tree" if directory else "blob"
    return f"{REPO}/{kind}/{commit}/{quote(path, safe='/')}"


def rewrite_url(url, source, page, commit):
    parsed = urlsplit(url)
    if parsed.scheme == "file":
        raise ValueError(f"{source}: file URL is not portable: {url}")
    if parsed.scheme or parsed.netloc or not parsed.path:
        return url
    path = unquote(parsed.path)
    if path.startswith("/") or "\\" in path:
        raise ValueError(f"{source}: use a relative repository link: {url}")
    resolved = (ROOT / source).parent / path
    resolved = resolved.resolve()
    if not resolved.is_relative_to(ROOT) or not resolved.exists():
        raise ValueError(f"{source}: missing or out-of-repository link: {url}")
    repository_path = resolved.relative_to(ROOT).as_posix()
    if subprocess.run(["git", "check-ignore", "--no-index", "-q", repository_path],
                      cwd=ROOT).returncode == 0:
        raise ValueError(f"{source}: link targets an ignored local file: {url}")
    if repository_path in DESTINATIONS:
        target = relative(DESTINATIONS[repository_path], page)
    else:
        target = source_url(repository_path, commit, resolved.is_dir())
    target_parts = urlsplit(target)
    return urlunsplit((*target_parts[:3], parsed.query, parsed.fragment))


class RewriteLinks(HTMLParser):
    """Rewrite rendered links, including those in raw Markdown HTML."""

    def __init__(self, source, page, commit):
        super().__init__(convert_charrefs=False)
        self.source, self.page, self.commit = source, page, commit
        self.output = []

    def start(self, tag, attrs, suffix):
        rewritten = []
        for key, value in attrs:
            if key in ("href", "src") and value is not None:
                value = rewrite_url(value, self.source, self.page, self.commit)
            rewritten.append(key if value is None else f'{key}="{html.escape(value, quote=True)}"')
        self.output.append("<" + tag + (" " if rewritten else "") + " ".join(rewritten) + suffix)

    def handle_starttag(self, tag, attrs):
        self.start(tag, attrs, ">")

    def handle_startendtag(self, tag, attrs):
        self.start(tag, attrs, " />")

    def handle_endtag(self, tag):
        self.output.append(f"</{tag}>")

    def handle_data(self, data):
        self.output.append(data)

    def handle_entityref(self, name):
        self.output.append(f"&{name};")

    def handle_charref(self, name):
        self.output.append(f"&#{name};")

    def handle_comment(self, data):
        self.output.append(f"<!--{data}-->")

    def handle_decl(self, decl):
        self.output.append(f"<!{decl}>")


def head(title, page, commit):
    assets = relative("assets", page)
    return f'''<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="thc-revision" content="{commit}">
<title>{html.escape(title)} · jam-vm</title>
<script src="{assets}/theme.js"></script>
<link rel="stylesheet" href="{assets}/site.css">
</head>'''


def nav(page, title):
    return (f'<a class="thc-nav-link" href="{page}" data-page="{page}" '
            f'target="thc-content">{html.escape(title)}</a>')


def shell(commit, preview):
    # Shell markup and theme hooks follow ekmett/thc's tools/docs/Main.hs;
    # NOTICE.md and docs/site/LICENSE-thc.txt preserve its attribution and terms.
    label = "Uncommitted preview · source links use main" if preview else f"Revision {commit[:12]}"
    links = "\n".join(nav(page, title) for _, page, title in PAGES[1:-1])
    inventory = json.dumps([page for _, page, _ in PAGES])
    return head("Documentation", "index.html", commit) + f'''
<body class="thc-shell">
<button class="thc-menu" type="button" aria-expanded="false" aria-controls="thc-rail">Documentation menu</button>
<div class="thc-shell-layout"><aside class="thc-rail" id="thc-rail">
<a class="thc-brand" href="home.html" data-page="home.html" target="thc-content">jam-vm<span> / docs</span></a>
<fieldset class="thc-appearance" id="thc-appearance"><legend>Appearance</legend>
<div class="thc-theme-options">
<button type="button" data-thc-appearance="light" aria-pressed="false">Light</button>
<button type="button" data-thc-appearance="dark" aria-pressed="false">Dark</button>
<button type="button" data-thc-appearance="system" aria-pressed="true">Follow OS</button>
</div></fieldset>
<nav aria-label="jam-vm documentation"><p class="thc-nav-label">Start</p>
{nav("home.html", "Home")}
<a class="thc-nav-link" href="{REPO}" target="_blank" rel="noopener noreferrer">GitHub ↗</a>
<p class="thc-nav-label">Guides</p>{links}</nav>
<div class="thc-rail-footer"><p>Experimental · {label}</p>
<a href="{REPO}/tree/{commit}" target="_blank" rel="noopener noreferrer">Source tree</a> ·
<a href="license.html" data-page="license.html" target="thc-content">License</a>
<p>Theme from <a href="https://github.com/ekmett/thc" target="_blank" rel="noopener noreferrer">thc</a> ·
<a href="assets/LICENSE-thc.txt" target="_blank" rel="noopener noreferrer">Theme license</a></p>
</div></aside>
<iframe id="thc-content" name="thc-content" title="jam-vm documentation content" src="home.html"></iframe>
</div><script type="application/json" id="thc-pages">{inventory}</script>
<script src="assets/site.js" defer></script></body></html>
'''


def build():
    commit, preview = revision()
    requested_pandoc = os.environ.get("PANDOC", "pandoc")
    pandoc = shutil.which(requested_pandoc)
    if pandoc is None:
        raise ValueError(f"Pandoc 3 executable not found: {requested_pandoc}")
    pandoc = str(Path(pandoc).resolve())
    version = subprocess.check_output([pandoc, "--version"], text=True).splitlines()[0]
    if not re.match(r"pandoc 3\.", version):
        raise ValueError(f"Pandoc 3 is required; found {version}")
    # Never replace an output directory redirected outside this checkout.
    if (ROOT / "build").is_symlink() or SITE.is_symlink() or SITE.resolve() != SITE:
        raise ValueError("Refusing redirected build/site output")
    rendered = {}
    for source, page, title in PAGES:
        body = subprocess.check_output(
            [pandoc, "--from=gfm", "--to=html5", "--wrap=none", source], cwd=ROOT, text=True)
        rewrite = RewriteLinks(source, page, commit)
        rewrite.feed(body)
        rewrite.close()
        notice = '<p>Uncommitted preview; source links use main.</p>' if preview else ""
        rendered[page] = head(title, page, commit) + f'''
<body class="thc-guide" data-thc-section="guide"><main class="thc-prose" id="main">
{"".join(rewrite.output)}
<footer class="thc-footer">{notice}
<a href="{relative('index.html', page)}">Documentation home</a> ·
<a href="{source_url(source, commit)}">View this page's source</a> ·
<a href="{relative('license.html', page)}">License</a>
</footer></main></body></html>
'''
    for name in ASSETS:
        if not (ROOT / "docs/site" / name).is_file():
            raise ValueError(f"Missing site asset: {name}")
    if SITE.exists():
        shutil.rmtree(SITE)
    (SITE / "assets").mkdir(parents=True)
    for name in ASSETS:
        shutil.copyfile(ROOT / "docs/site" / name, SITE / "assets" / name)
    rendered["index.html"] = shell(commit, preview)
    for page, content in rendered.items():
        target = SITE / page
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
    (SITE / "revision.txt").write_text(commit + "\n", encoding="utf-8")
    (SITE / ".nojekyll").touch()
    print(f"Built {len(rendered)} HTML pages with {version}: {SITE}")
    print("Uncommitted preview; source links use main." if preview else f"Source revision: {commit}")


if __name__ == "__main__":
    try:
        build()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f"docs build: {error}")
