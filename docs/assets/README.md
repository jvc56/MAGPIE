# Assets

Drop site imagery here and wire it up in `mkdocs.yml`:

- **`logo.svg`** → `theme.logo: assets/logo.svg`
- **`favicon.png`** → `theme.favicon: assets/favicon.png`
- Social-card defaults (optional) → configure under the `social` plugin.

Until a logo/favicon is added, Material uses its default mark. The `social`
plugin auto-generates per-page Open Graph / Twitter preview cards at build time
in CI (it needs Cairo/Pango, installed by `.github/workflows/docs.yml`).

This file is a placeholder so the directory is tracked; it is not in the nav.
