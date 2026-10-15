import type {ReactNode} from 'react';
import Link from '@docusaurus/Link';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import Layout from '@theme/Layout';
import Heading from '@theme/Heading';
import CodeBlock from '@theme/CodeBlock';

import styles from './index.module.css';

const functionGroups = [
  'ai_complete',
  'ai_classify',
  'ai_filter',
  'ai_summarize',
  'ai_extract_record',
  'ai_embed',
  'ai_similarity',
  'ai_query_data',
  'ai_usage',
];

const quickstart = `INSTALL ai FROM community;
LOAD ai;

SET duckdb_ai_provider = 'ollama';
SET duckdb_ai_model = 'qwen3.8:27b';

SELECT ticket,
       ai_classify(ticket, ['billing', 'performance', 'other']) AS category
FROM (VALUES ('I was charged twice.'), ('My query got slow.')) AS t(ticket);`;

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
              duckdb-ai is a DuckDB extension that calls LLMs from SQL. Classify,
              summarize and filter rows, extract typed fields with a JSON Schema,
              generate embeddings for semantic search, and turn questions into
              read-only SQL. Use local models through Ollama or llama.cpp, or
              hosted providers such as OpenAI, Claude, Gemini, Bedrock,
              Databricks and Snowflake Cortex.
            </p>
            <CodeBlock language="sql">{quickstart}</CodeBlock>
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
                <Link to="/docs/agent-guide">Agent guide</Link>
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
  return (
    <Layout
      title="LLMs and embeddings in DuckDB SQL"
      description="Call LLMs from DuckDB SQL: classify, summarize and extract typed data, generate embeddings for semantic search, and turn questions into SQL with Ollama, OpenAI, Claude, Gemini and 30+ providers.">
      <HomepageHeader />
      <main>
        <DocsSummary />
      </main>
    </Layout>
  );
}
