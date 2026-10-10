# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: UPL-1.0 AND BSD-3-Clause
"""Assemble Pandoc guides and Doxygen reference in the shared THC docs layout."""
import argparse
import html
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
from urllib.parse import unquote, urlsplit, urlunsplit

from check_links import check
from fix_module_anchors import repair


def relative(path, page):
    return os.path.relpath(path, page.parent).replace(os.sep, '/')


def build(source, api, output, name, pandoc):
    source, api, output = (p.resolve() for p in (source, api, output))
    if output == source or output in source.parents or output == api or output in api.parents:
        raise ValueError('Site output must be separate from source and API input')
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    repo = f'https://github.com/ekmett/{name}'
    guides = [Path('README.md'), Path('LICENSE.md')]
    if (source / 'THIRD_PARTY_NOTICES.md').is_file():
        guides.append(Path('THIRD_PARTY_NOTICES.md'))
    guides += sorted(p.relative_to(source) for p in (source / 'docs').rglob('*.md'))
    guides += [p for p in (Path('vm/README.md'), Path('vm/LICENSE.md'), Path('vm/NOTICE.md')) if (source / p).is_file()]
    routes = {p: Path('home.html') if p == Path('README.md') else Path('guides') / p.with_suffix('.html') for p in guides}
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    shutil.copytree(api, output / 'api')
    repair(output / 'api')
    assets = output / 'assets'
    assets.mkdir()
    for file in ('site.css', 'reference.css', 'theme.js', 'site.js'):
        shutil.copyfile(source / 'docs/site' / file, assets / file)
    for directory in ('images', 'badges'):
        if (source / 'assets' / directory).is_dir():
            shutil.copytree(source / 'assets' / directory, assets / directory)
    icon = '<link rel="icon" href="assets/images/favicon.png" type="image/png">' if (assets / 'images/favicon.png').is_file() else ''

    def decorate(text, page, reference=False):
        tags = f'<meta name="docs-revision" content="{revision}">'
        tags += f'<script src="{relative(Path("assets/theme.js"), page)}?v={revision}"></script>'
        tags += f'<link rel="stylesheet" href="{relative(Path("assets/site.css"), page)}?v={revision}">'
        if reference:
            tags += f'<link rel="stylesheet" href="{relative(Path("assets/reference.css"), page)}?v={revision}">'
        if '</head>' not in text:
            raise ValueError(f'Missing HTML head: {page}')
        return text.replace('</head>', tags + '</head>', 1)

    for page in (output / 'api').rglob('*.html'):
        page.write_text(decorate(page.read_text(), page.relative_to(output), True))

    def rewrite(url, original, page):
        parsed = urlsplit(html.unescape(url))
        if parsed.scheme or parsed.netloc or not parsed.path:
            return url
        target = (source / original.parent / unquote(parsed.path)).resolve()
        if not target.is_relative_to(source):
            raise ValueError(f'Guide link escapes repository: {url}')
        local = target.relative_to(source)
        if local in routes:
            path = relative(routes[local], page)
        elif local.parts[:2] in (('assets', 'images'), ('assets', 'badges')) and target.is_file():
            path = relative(local, page)
        else:
            path = f'{repo}/blob/{revision}/{local.as_posix()}'
        return html.escape(urlunsplit(('', '', path, parsed.query, parsed.fragment)), quote=True)

    navigation = []
    for original, page in routes.items():
        text = (source / original).read_text()
        heading = re.search(r'^#\s+(.+)', text, re.M)
        title = re.sub(r'\s*<!--.*?-->', '', heading[1]) if heading else original.stem
        fragment = subprocess.check_output([pandoc, '--from=gfm', '--to=html5', '--wrap=none', '--syntax-highlighting=none', str(source / original)], text=True)
        fragment = re.sub(r'\b(href|src)="([^"]*)"', lambda m: f'{m[1]}="{rewrite(m[2], original, page)}"', fragment)
        content = f'''<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>{html.escape(title)} · {name}</title></head>
<body class="thc-guide"><main class="thc-prose">{fragment}
<footer class="thc-footer"><a href="{repo}/blob/{revision}/{original.as_posix()}">View this page’s source</a></footer></main></body></html>'''
        destination = output / page
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(decorate(content, page))
        if original != Path('README.md'):
            navigation.append((page.as_posix(), title))

    def item(page, title):
        return f'<a class="thc-nav-link" data-page="{page}" href="{page}" target="thc-content">{html.escape(title)}</a>'

    pages = sorted(p.relative_to(output).as_posix() for p in output.rglob('*.html'))
    nav = ''.join(item(page, title) for page, title in navigation)
    reference = 'api/annotated.html' if (output / 'api/annotated.html').exists() else 'api/files.html'
    reference_nav = item(reference, 'API reference')
    if reference != 'api/files.html':
        reference_nav += item('api/files.html', 'Source files')
    manifest = json.dumps(pages).replace('<', '\\u003c')
    shell = f'''<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="docs-revision" content="{revision}"><meta name="docs-project" content="{name}"><title>{name} documentation</title>{icon}
<script src="assets/theme.js?v={revision}"></script><link rel="stylesheet" href="assets/site.css?v={revision}"></head>
<body class="thc-shell"><button class="thc-menu" type="button" aria-expanded="false" aria-controls="thc-rail">Documentation menu</button>
<div class="thc-shell-layout"><aside class="thc-rail" id="thc-rail">
<a class="thc-brand" href="home.html" data-page="home.html" target="thc-content">{name}<span> / docs</span></a>
<fieldset class="thc-appearance"><legend>Appearance</legend><div class="thc-theme-options">
<button type="button" data-thc-appearance="light" aria-pressed="false">Light</button>
<button type="button" data-thc-appearance="dark" aria-pressed="false">Dark</button>
<button type="button" data-thc-appearance="system" aria-pressed="true">Follow OS</button></div></fieldset>
<nav aria-label="{name} documentation"><p class="thc-nav-label">Start</p>{item('home.html', 'Overview')}
<a class="thc-nav-link" href="{repo}" target="_blank" rel="noopener noreferrer">GitHub ↗</a>
<p class="thc-nav-label">Guides</p>{nav}<p class="thc-nav-label">Reference</p>
{reference_nav}</nav>
<div class="thc-rail-footer"><a href="{repo}/commit/{revision}">Source · {revision[:12]}</a></div></aside>
<iframe id="thc-content" name="thc-content" title="{name} documentation content" src="home.html?v={revision}"></iframe></div>
<script type="application/json" id="thc-pages">{manifest}</script><script src="assets/site.js?v={revision}" defer></script></body></html>'''
    (output / 'index.html').write_text(shell)
    (output / '.nojekyll').touch()
    if check(output):
        raise SystemExit('Broken links in assembled documentation')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('api', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--name', required=True)
    parser.add_argument('--pandoc', default='pandoc')
    args = parser.parse_args()
    build(args.source, args.api, args.output, args.name, args.pandoc)
