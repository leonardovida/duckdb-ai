import type {SidebarsConfig} from '@docusaurus/plugin-content-docs';

// This runs in Node.js - Don't use client-side code here (browser APIs, JSX...)

/**
 * Creating a sidebar enables you to:
 - create an ordered group of docs
 - render a sidebar for each doc of that group
 - provide next/previous navigation

 The sidebars can be generated from the filesystem, or explicitly defined here.

 Create as many sidebars as you want.
 */
const sidebars: SidebarsConfig = {
  docsSidebar: [
    'index',
    {
      type: 'category',
      label: 'Cookbooks',
      link: {
        type: 'doc',
        id: 'cookbooks/index',
      },
      items: [
        'cookbooks/support-ticket-data',
        'cookbooks/support-ticket-enrichment',
        'cookbooks/support-ticket-similarity',
        'cookbooks/structured-triage-records',
        'cookbooks/sql-assistant',
      ],
    },
    'functions',
    'provider-guides',
    'best-practices',
    'runtime-behavior',
    'security-data-flow',
    'VALIDATION',
    'SMOKE_TESTING',
    'DISTRIBUTION',
    'RELEASING',
    'UPDATING',
  ],
};

export default sidebars;
