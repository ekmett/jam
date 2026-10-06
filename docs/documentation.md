# Building the documentation

Install **Python 3.10+**, **Pandoc 3.x** and Make. From the repository root:

```sh
make docs
make docs-check
python3 -m http.server 8000 --directory build/site
```

Open `http://localhost:8000/` to preview the site. `make docs` renders and checks
the output; `make docs-check` checks an existing build. Set `PANDOC` or `PYTHON`
to select a different executable:

```sh
make docs PANDOC=/absolute/path/to/pandoc PYTHON=python3
```

The site is written to `build/site/`. Its links are relative, so the same output
works at a server root or under `/jam-vm/`. The shared navigation preserves the
selected page and anchor in the browser URL. Individual HTML pages also work
when opened directly.

## Editing guides

Edit the Markdown in `docs/`. The front page and shared assets live in
`docs/site/`; [the builder](../tools/build_docs.py) contains the page list and
navigation labels. Add a guide there to include it in the site.

Use repository-relative Markdown links. The builder maps published guides to
their HTML pages and other repository files to GitHub. The site includes only
the selected guides and assets. Missing or ignored targets fail the build.

The checker validates the page inventory, local links, anchors, shared assets
and source revision. It does not contact external websites. Browser behavior
still deserves a look when changing the shell or appearance controls.

## Source revisions

A clean committed checkout uses its exact commit for source links. A dirty
checkout, or one with no commits yet, is labelled an uncommitted preview and
links to `main`. Those links cannot show changes that have not been pushed.

CI sets `DOCS_REVISION` to the triggering commit. The builder requires it to
match `HEAD`, and the checker verifies that the resulting pages carry that
revision. This keeps a built artifact tied to its source.

## CI

The [Documentation workflow](../.github/workflows/docs.yml) runs on pushes to
`main`, pull requests and manual dispatch. It installs Pandoc,
runs `make docs`, and uploads `build/site/` as the `jam-vm-docs` artifact.
It needs no C++ compiler, GraalVM installation or prepared upstream sources.

Builds from `main` publish the checked site to
[ekmett.github.io/jam-vm](https://ekmett.github.io/jam-vm/).
Pull requests build and check the same site without deploying it.
The repository's Pages publishing source must be set to **GitHub Actions**.
