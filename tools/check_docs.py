# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

"""Validate the built site's page inventory, portable links and HTML anchors."""

from html.parser import HTMLParser
import json
import os
import re
import sys
from urllib.parse import unquote, urlsplit

from build_docs import ASSETS, PAGES, SITE


class Document(HTMLParser):
    def __init__(self, path):
        super().__init__(convert_charrefs=True)
        self.ids = set()
        self.urls = []
        self.revision = None
        self.inventory = ""
        self.in_inventory = False
        self.routes = []
        self.iframe = None
        self.feed(path.read_text(encoding="utf-8"))
        self.close()

    def handle_starttag(self, tag, attributes):
        attrs = dict(attributes)
        if "id" in attrs:
            if attrs["id"] in self.ids:
                raise ValueError(f"Duplicate HTML id: {attrs['id']}")
            self.ids.add(attrs["id"])
        if tag == "a" and "name" in attrs:
            self.ids.add(attrs["name"])
        self.urls.extend(attrs[name] for name in ("href", "src") if attrs.get(name))
        if tag == "meta" and attrs.get("name") == "thc-revision":
            self.revision = attrs.get("content")
        if tag == "script" and attrs.get("id") == "thc-pages":
            self.in_inventory = True
        if "data-page" in attrs:
            if attrs.get("href") != attrs["data-page"]:
                raise ValueError("Shell route differs from its href")
            self.routes.append(attrs["data-page"])
        if tag == "iframe":
            self.iframe = attrs

    def handle_endtag(self, tag):
        if tag == "script":
            self.in_inventory = False

    def handle_data(self, data):
        if self.in_inventory:
            self.inventory += data


def check():
    revision = (SITE / "revision.txt").read_text(encoding="utf-8").strip()
    if revision != "main" and not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("Invalid generated revision")
    if os.environ.get("DOCS_REVISION", revision) != revision:
        raise ValueError("Artifact revision differs from DOCS_REVISION")
    expected = {page for _, page, _ in PAGES} | {"index.html"}
    actual = {path.relative_to(SITE).as_posix() for path in SITE.rglob("*.html")}
    if actual != expected:
        raise ValueError(f"HTML inventory differs: missing {expected - actual}, extra {actual - expected}")
    for name in (*[f"assets/{asset}" for asset in ASSETS], ".nojekyll"):
        if not (SITE / name).is_file():
            raise ValueError(f"Missing generated file: {name}")
    documents = {page: Document(SITE / page) for page in sorted(actual)}
    shell = documents["index.html"]
    inventory = [page for _, page, _ in PAGES]
    if json.loads(shell.inventory) != inventory or set(shell.routes) != set(inventory):
        raise ValueError("Shell route inventory differs from the published pages")
    if not {"thc-rail", "thc-content", "thc-appearance", "thc-pages"} <= shell.ids:
        raise ValueError("Shell is missing a theme or navigation hook")
    if "assets/site.js" not in shell.urls:
        raise ValueError("Shell navigation script missing")
    if not shell.iframe or shell.iframe.get("src") != "home.html" or shell.iframe.get("name") != "thc-content":
        raise ValueError("Shell iframe must initially show home.html")
    links = 0
    for page, document in documents.items():
        if document.revision != revision:
            raise ValueError(f"{page}: revision metadata differs")
        if not any(url.endswith("assets/theme.js") for url in document.urls) or not any(
                url.endswith("assets/site.css") for url in document.urls):
            raise ValueError(f"{page}: shared theme assets missing")
        for url in document.urls:
            links += 1
            parsed = urlsplit(url)
            if parsed.scheme == "file":
                raise ValueError(f"{page}: file URL: {url}")
            if parsed.scheme or parsed.netloc:
                continue
            path = unquote(parsed.path)
            if path.startswith("/") or "\\" in path:
                raise ValueError(f"{page}: non-portable local URL: {url}")
            target = ((SITE / page).parent / path).resolve() if path else SITE / page
            if not target.is_relative_to(SITE) or not target.is_file():
                raise ValueError(f"{page}: missing or out-of-site link: {url}")
            target_page = target.relative_to(SITE).as_posix()
            if parsed.fragment and target_page in documents:
                if unquote(parsed.fragment) not in documents[target_page].ids:
                    raise ValueError(f"{page}: missing anchor: {url}")
    print(f"Checked {len(documents)} HTML pages, {links} links, shell routes and anchors ({revision}).")


if __name__ == "__main__":
    try:
        check()
    except (OSError, ValueError) as error:
        sys.exit(f"docs check: {error}")
