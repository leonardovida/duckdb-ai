import {themes as prismThemes} from 'prism-react-renderer';
import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

import llmsTxtPlugin from './plugins/llms-txt';
import sidebars from './sidebars';

const siteDescription =
  'duckdb-ai is a DuckDB extension that calls LLMs from SQL: classify, summarize ' +
  'and extract typed data, generate embeddings for semantic search, and turn ' +
  'questions into read-only SQL, with Ollama, OpenAI, Claude, Gemini and 30+ providers.';

// This runs in Node.js - Don't use client-side code here (browser APIs, JSX...)

const config: Config = {
  title: 'DuckDB AI extension',
  tagline: 'Call LLMs and embedding models from DuckDB SQL',

  // Future flags, see https://docusaurus.io/docs/api/docusaurus-config#future
  future: {
    v4: true, // Improve compatibility with the upcoming Docusaurus v4
  },

  // Set the production url of your site here
  url: 'https://leonardovida.github.io',
  // Set the /<baseUrl>/ pathname under which your site is served
  // For GitHub pages deployment, it is often '/<projectName>/'
  baseUrl: '/duckdb-ai/',

  // GitHub pages deployment config.
  // If you aren't using GitHub pages, you don't need these.
  organizationName: 'leonardovida', // Usually your GitHub org/user name.
  projectName: 'duckdb-ai', // Usually your repo name.
  trailingSlash: false,

  onBrokenLinks: 'throw',
  markdown: {
    hooks: {
      onBrokenMarkdownLinks: 'warn',
    },
  },

  // Even if you don't use internationalization, you can use this field to set
  // useful metadata like html lang. For example, if your site is Chinese, you
  // may want to replace "en" with "zh-Hans".
  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  headTags: [
    {
      tagName: 'script',
      attributes: {type: 'application/ld+json'},
      innerHTML: JSON.stringify({
        '@context': 'https://schema.org',
        '@type': 'SoftwareSourceCode',
        name: 'duckdb-ai',
        alternateName: 'DuckDB AI extension',
        description: siteDescription,
        url: 'https://leonardovida.github.io/duckdb-ai/',
        codeRepository: 'https://github.com/leonardovida/duckdb-ai',
        programmingLanguage: ['SQL', 'C++'],
        runtimePlatform: 'DuckDB',
        license: 'https://opensource.org/licenses/MIT',
        keywords:
          'DuckDB, LLM, AI SQL functions, embeddings, semantic search, text-to-SQL, Ollama, OpenAI, Claude, Gemini',
      }),
    },
  ],

  plugins: [
    [
      llmsTxtPlugin,
      {
        docsDir: '../docs',
        sidebar: sidebars.docsSidebar,
        summary: siteDescription,
      },
    ],
  ],

  presets: [
    [
      'classic',
      {
        docs: {
          path: '../docs',
          routeBasePath: 'docs',
          sidebarPath: './sidebars.ts',
          editUrl:
            'https://github.com/leonardovida/duckdb-ai/tree/main/',
        },
        blog: false,
        theme: {
          customCss: './src/css/custom.css',
        },
      } satisfies Preset.Options,
    ],
  ],

  themeConfig: {
    metadata: [
      {name: 'description', content: siteDescription},
      {
        name: 'keywords',
        content:
          'DuckDB AI, DuckDB LLM, DuckDB extension, LLM in SQL, AI SQL functions, DuckDB embeddings, semantic search, text-to-SQL, Ollama, OpenAI, Claude, Gemini',
      },
      {property: 'og:type', content: 'website'},
      {property: 'og:site_name', content: 'duckdb-ai'},
      {name: 'twitter:card', content: 'summary'},
    ],
    colorMode: {
      respectPrefersColorScheme: true,
    },
    navbar: {
      title: 'duckdb-ai',
      items: [
        {
          type: 'docSidebar',
          sidebarId: 'docsSidebar',
          position: 'left',
          label: 'Docs',
        },
        {
          href: 'https://github.com/leonardovida/duckdb-ai',
          label: 'GitHub',
          position: 'right',
        },
      ],
    },
    footer: {
      style: 'dark',
      links: [
        {
          title: 'Docs',
          items: [
            {
              label: 'Overview',
              to: '/docs',
            },
            {
              label: 'SQL reference',
              to: '/docs/functions',
            },
            {
              label: 'Provider guides',
              to: '/docs/provider-guides',
            },
          ],
        },
        {
          title: 'Project',
          items: [
            {
              label: 'GitHub',
              href: 'https://github.com/leonardovida/duckdb-ai',
            },
          ],
        },
        {
          title: 'More',
          items: [
            {
              label: 'Cookbooks',
              to: '/docs/cookbooks',
            },
            {
              label: 'Security and data flow',
              to: '/docs/security-data-flow',
            },
          ],
        },
      ],
      copyright: `Copyright © ${new Date().getFullYear()} duckdb-ai contributors. Built with Docusaurus.`,
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
    },
  } satisfies Preset.ThemeConfig,
};

export default config;
