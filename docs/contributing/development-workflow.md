# Development Workflow

This page defines the expected local workflow before opening a pull request.

## 1) Fork and create a focused branch

- Fork the repository to your own GitHub account
- Clone your fork locally and add the upstream repository if needed
- Enable repo hooks once per clone: `git config core.hooksPath .githooks && chmod +x .githooks/pre-commit`

- Branch from `main`
- Keep each PR focused on one fix or feature area

## 2) Implement with scope in mind

- Confirm your idea is in project scope: [SCOPE.md](../../SCOPE.md)
- Prefer incremental changes over broad refactors

## 3) Run local checks

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run                       # upstream default (X4 / ESP32-C3) target that CI builds
pio run -e onepage            # OnePage / ESP32-C61 target of this repo
```

If you touched anything under `src/network/html/`, also run
`python3 scripts/sync_web_core.py` and `python3 scripts/build_html.py`
(`--check` on the former is the drift gate).

CI enforces formatting, static analysis, and build checks.
Use clang-format 21+ locally to match CI.
If `clang-format` is missing or too old locally, see [Getting Started](./getting-started.md).

## 4) Open the PR

- Use a semantic title (example: `fix: avoid crash when opening malformed epub`)
- Fill out `.github/PULL_REQUEST_TEMPLATE.md`
- Describe the problem, approach, and any tradeoffs
- Include reproduction and verification steps for bug fixes

## 5) Review etiquette

- Be explicit and concise in responses
- Keep discussions technical and respectful
- Assume good intent and focus on code-level feedback

For community expectations, see [GOVERNANCE.md](../../GOVERNANCE.md).
