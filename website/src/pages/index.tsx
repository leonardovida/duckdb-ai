import type {ReactNode} from 'react';
import Link from '@docusaurus/Link';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import Layout from '@theme/Layout';
import Heading from '@theme/Heading';

import styles from './index.module.css';

const functionGroups = [
  'ai_complete',
  'ai_complete_json',
  'ai_complete_record',
  'ai_embed',
  'ai_filter',
  'ai_sql',
  'ai_query_data',
  'ai_count_tokens',
  'ai_usage',
];

function HomepageHeader(): ReactNode {
  const {siteConfig} = useDocusaurusContext();
  return (
    <header className={styles.hero}>
      <div className="container">
        <Heading as="h1" className={styles.title}>
          {siteConfig.title}
        </Heading>
        <p className={styles.subtitle}>{siteConfig.tagline}</p>
        <div className={styles.buttons}>
          <Link
            className="button button--primary button--lg"
            to="/docs">
            Read the docs
          </Link>
          <Link
            className="button button--secondary button--lg"
            to="https://github.com/leonardovida/duckdb-ai">
            View on GitHub
          </Link>
        </div>
      </div>
    </header>
  );
}

function DocsSummary(): ReactNode {
  return (
    <section className={styles.summary}>
      <div className="container">
        <div className={styles.grid}>
          <article>
            <Heading as="h2">What it covers</Heading>
            <p>
              <code>duckdb_ai</code> adds SQL functions for completion models,
              structured JSON output, embeddings, generated read-only SQL,
              usage logging, and provider metadata.
            </p>
            <div className={styles.chips}>
              {functionGroups.map((name) => (
                <code key={name}>{name}</code>
              ))}
            </div>
          </article>
          <article>
            <Heading as="h2">Current status</Heading>
            <p>
              The extension is source-first while the public SQL surface and
              provider configuration contract settle. The docs include local
              function reference, validation evidence, smoke-test commands, and
              release notes.
            </p>
            <ul className={styles.linkList}>
              <li>
                <Link to="/docs/functions">SQL function reference</Link>
              </li>
              <li>
                <Link to="/docs/VALIDATION">Validation evidence</Link>
              </li>
              <li>
                <Link to="/docs/SMOKE_TESTING">Smoke testing</Link>
              </li>
              <li>
                <Link to="/docs/DISTRIBUTION">Distribution status</Link>
              </li>
            </ul>
          </article>
        </div>
      </div>
    </section>
  );
}

export default function Home(): ReactNode {
  const {siteConfig} = useDocusaurusContext();
  return (
    <Layout
      title={siteConfig.title}
      description="Documentation for the duckdb_ai DuckDB extension">
      <HomepageHeader />
      <main>
        <DocsSummary />
      </main>
    </Layout>
  );
}
