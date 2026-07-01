# duckdb-ai docs site

This directory contains the Docusaurus site shell for the duckdb-ai
documentation. The published docs content is sourced from the repository-level
`docs/` directory.

## Local development

```sh
npm install
npm run start
```

## Build

```sh
npm run typecheck
npm run build
```

The production build output is written to `website/build/`.

## Deployment

GitHub Pages deployment is handled by
`.github/workflows/deploy-docs.yml`. Pull requests run typecheck and build.
Pushes to `main` upload `website/build/` as a GitHub Pages artifact and deploy
it to:

```text
https://leonardovida.github.io/duckdb-ai/
```
