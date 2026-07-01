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
              duckdb-ai adds SQL functions for completion models, structured
              JSON output, embeddings, generated read-only SQL, usage logging,
              and provider metadata.
            </p>
            <div className={styles.chips}>
              {functionGroups.map((name) => (
                <code key={name}>{name}</code>
              ))}
            </div>
          </article>
          <article>
            <Heading as="h2">Start here</Heading>
            <p>
              Use the reference pages and cookbooks to choose a provider,
              configure credentials, call models from SQL, and understand how
              provider data moves through the extension.
            </p>
            <ul className={styles.linkList}>
              <li>
                <Link to="/docs/functions">SQL function reference</Link>
              </li>
              <li>
                <Link to="/docs/provider-guides">Provider guides</Link>
              </li>
              <li>
                <Link to="/docs/cookbooks">Cookbooks</Link>
              </li>
              <li>
                <Link to="/docs/security-data-flow">Security and data flow</Link>
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
      description="Documentation for the duckdb-ai DuckDB extension">
      <HomepageHeader />
      <main>
        <DocsSummary />
      </main>
    </Layout>
  );
}
