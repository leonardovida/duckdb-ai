import fs from 'node:fs/promises';
import path from 'node:path';
import type {LoadContext, Plugin} from '@docusaurus/types';

// Writes llms.txt (page index) and llms-full.txt (full Markdown) into the build
// output, following https://llmstxt.org. Both files are regenerated from
// ../docs on every build, in sidebar order, so they cannot drift from the site.

type SidebarItem =
  | string
  | {type: string; link?: {type: string; id?: string}; items?: SidebarItem[]};

type Options = {
  docsDir: string;
  sidebar: SidebarItem[];
  summary: string;
};

type Page = {id: string; url: string; title: string; description: string; body: string};

function collectIds(items: SidebarItem[]): string[] {
  return items.flatMap((item) => {
    if (typeof item === 'string') {
      return [item];
    }
    const ids: string[] = [];
    if (item.link?.type === 'doc' && item.link.id) {
      ids.push(item.link.id);
    }
    return ids.concat(collectIds(item.items ?? []));
  });
}

function frontMatterValue(frontMatter: string, key: string): string | undefined {
  const match = frontMatter.match(new RegExp(`^${key}:\\s*(.+)$`, 'm'));
  if (!match) {
    return undefined;
  }
  return match[1].trim().replace(/^"(.*)"$/, '$1').replace(/\\"/g, '"');
}

async function readPage(docsDir: string, id: string, siteUrl: string): Promise<Page> {
  const source = await fs.readFile(path.join(docsDir, `${id}.md`), 'utf8');
  const match = source.match(/^---\n([\s\S]*?)\n---\n/);
  const frontMatter = match ? match[1] : '';
  const body = (match ? source.slice(match[0].length) : source).trim();
  const heading = body.match(/^# (.+)$/m)?.[1].trim();
  const slug = frontMatterValue(frontMatter, 'slug');
  const route = slug === '/' ? '' : `/${id.replace(/\/index$/, '')}`;
  return {
    id,
    url: `${siteUrl}docs${route}`,
    title: heading ?? frontMatterValue(frontMatter, 'title') ?? id,
    description: frontMatterValue(frontMatter, 'description') ?? '',
    body,
  };
}

export default function llmsTxtPlugin(context: LoadContext, rawOptions: unknown): Plugin {
  const options = rawOptions as Options;
  return {
    name: 'llms-txt',
    async postBuild({outDir}) {
      const siteUrl = `${context.siteConfig.url}${context.baseUrl}`;
      const ids = collectIds(options.sidebar);
      const pages = await Promise.all(ids.map((id) => readPage(options.docsDir, id, siteUrl)));
      const title = context.siteConfig.title;

      const index = [
        `# ${title}`,
        '',
        `> ${options.summary}`,
        '',
        `Full text of every page: ${siteUrl}llms-full.txt`,
        '',
        '## Docs',
        '',
        ...pages.map((page) =>
          `- [${page.title}](${page.url})${page.description ? `: ${page.description}` : ''}`,
        ),
        '',
        '## Source',
        '',
        '- [GitHub repository](https://github.com/leonardovida/duckdb-ai)',
        '- [Changelog](https://github.com/leonardovida/duckdb-ai/blob/main/CHANGELOG.md)',
        '- [DuckDB community extension page](https://duckdb.org/community_extensions/extensions/ai)',
        '',
      ].join('\n');

      const full = [
        `# ${title}`,
        '',
        `> ${options.summary}`,
        '',
        ...pages.map((page) => `<!-- Source: ${page.url} -->\n\n${page.body}\n`),
      ].join('\n');

      await fs.writeFile(path.join(outDir, 'llms.txt'), index);
      await fs.writeFile(path.join(outDir, 'llms-full.txt'), full);
    },
  };
}
