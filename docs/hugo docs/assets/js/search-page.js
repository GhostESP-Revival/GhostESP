(() => {
  const root = document.querySelector('[data-search-page]');
  if (!root) return;

  const input = root.querySelector('[data-search-page-input]');
  const versionSelect = root.querySelector('[data-search-page-version]');
  const resultsEl = root.querySelector('[data-search-page-results]');
  const statusEl = root.querySelector('[data-search-page-status]');
  const form = root.querySelector('[data-search-page-form]');
  if (!input || !resultsEl) return;

  const indexUrl = root.dataset.searchIndex || 'search-index.json';
  const MIN_QUERY_LENGTH = 2;
  const MAX_RESULTS = 40;

  let fuse = null;
  let items = [];
  let loading = false;
  let hasLoaded = false;
  let debounceTimer = null;
  let lastState = { q: '', version: '' };

  const escapeHtml = (text) => {
    const div = document.createElement('div');
    div.textContent = text == null ? '' : String(text);
    return div.innerHTML;
  };

  const highlight = (text, query) => {
    const escaped = escapeHtml(text);
    const q = (query || '').trim();
    if (!q) return escaped;
    const pattern = q.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    try {
      return escaped.replace(new RegExp(`(${pattern})`, 'gi'), '<mark>$1</mark>');
    } catch (error) {
      return escaped;
    }
  };

  const getParam = (name) => new URLSearchParams(window.location.search).get(name) || '';

  const syncUrl = (q, version) => {
    if (q === lastState.q && version === lastState.version) return;
    lastState = { q, version };
    const params = new URLSearchParams();
    if (q) params.set('q', q);
    if (version) params.set('version', version);
    const search = params.toString();
    const url = `${window.location.pathname}${search ? `?${search}` : ''}`;
    window.history.replaceState(null, '', url);
  };

  const matchingHeadings = (item, query) => {
    const headings = Array.isArray(item.headings) ? item.headings : [];
    const q = (query || '').trim().toLowerCase();
    if (!q) return [];
    return headings
      .filter((heading) => (heading.title || '').toLowerCase().includes(q))
      .slice(0, 4);
  };

  const buildHref = (item, query) => {
    const hits = matchingHeadings(item, query);
    if (hits.length) {
      return `${item.url}#${hits[0].id}`;
    }
    return item.url;
  };

  const snippetFor = (result, query) => {
    const item = result.item;
    const q = (query || '').trim().toLowerCase();
    const candidates = [item.summary, item.description, item.content].filter(Boolean);
    for (const text of candidates) {
      const lower = text.toLowerCase();
      const index = q ? lower.indexOf(q) : -1;
      if (index !== -1) {
        const start = Math.max(0, index - 90);
        const end = Math.min(text.length, index + q.length + 110);
        return `${start > 0 ? '…' : ''}${text.slice(start, end).replace(/\s+/g, ' ').trim()}${end < text.length ? '…' : ''}`;
      }
    }
    return item.summary || item.description || '';
  };

  const renderBrowse = (query) => {
    const version = versionSelect ? versionSelect.value : '';
    const scoped = items.filter((item) => !version || item.version === version);
    const seen = new Map();
    scoped.forEach((item) => {
      if (item.section && item.section_url && !seen.has(item.section_url)) {
        seen.set(item.section_url, item.section);
      }
    });
    const sections = Array.from(seen.entries()).slice(0, 30);
    if (!sections.length) {
      resultsEl.innerHTML = '';
      return;
    }
    resultsEl.innerHTML = `
      <div class="search-browse">
        <h2>Browse ${version ? escapeHtml(version) : 'latest'} sections</h2>
        <div class="search-browse__chips">
          ${sections.map(([url, label]) => `<a class="search-browse__chip" href="${escapeHtml(url)}">${escapeHtml(label)}</a>`).join('')}
        </div>
      </div>`;
  };

  const renderResults = (results, query) => {
    if (!results.length) {
      resultsEl.innerHTML = '<div class="search-empty">No matching pages. Try a shorter or different query.</div>';
      return;
    }

    resultsEl.innerHTML = results.slice(0, MAX_RESULTS).map((result) => {
      const item = result.item;
      const headingHits = matchingHeadings(item, query);
      const headingLinks = headingHits.length
        ? `<div class="search-result__headings">${headingHits
            .map((heading) => `<a href="${escapeHtml(item.url)}#${escapeHtml(heading.id)}">${highlight(heading.title, query)}</a>`)
            .join('')}</div>`
        : '';
      const textLink = item.text_url
        ? `<a class="search-result__text" href="${escapeHtml(item.text_url)}">plain text</a>`
        : '';
      const section = item.section ? escapeHtml(item.section) : 'Docs';
      return `
        <article class="search-result">
          <div class="search-result__top">
            <h3 class="search-result__title"><a href="${escapeHtml(buildHref(item, query))}">${highlight(item.title, query)}</a></h3>
            <span class="search-result__version">${escapeHtml(item.version || '')}</span>
          </div>
          <p class="search-result__crumb">${section}</p>
          <p class="search-result__snippet">${highlight(snippetFor(result, query), query)}</p>
          ${headingLinks}
          ${textLink}
        </article>`;
    }).join('');
  };

  const run = (query, usePush) => {
    const q = (query || '').trim();
    const version = versionSelect ? versionSelect.value : '';
    syncUrl(q, version);

    if (q.length < MIN_QUERY_LENGTH) {
      if (statusEl) statusEl.textContent = '';
      renderBrowse(q);
      return;
    }

    if (!fuse) {
      if (statusEl) statusEl.textContent = 'Loading search index…';
      return;
    }

    let results = fuse.search(q);
    if (version) {
      results = results.filter((result) => result.item.version === version);
    }

    if (statusEl) {
      const shown = Math.min(results.length, MAX_RESULTS);
      statusEl.textContent = results.length
        ? `${results.length} result${results.length === 1 ? '' : 's'}${results.length > shown ? ` (showing first ${shown})` : ''}`
        : '';
    }
    renderResults(results, q);
  };

  const debouncedRun = (query) => {
    clearTimeout(debounceTimer);
    debounceTimer = setTimeout(() => run(query), 250);
  };

  const loadIndex = async () => {
    if (hasLoaded || loading) return;
    loading = true;
    if (statusEl) statusEl.textContent = 'Loading search index…';
    try {
      const response = await fetch(indexUrl, { credentials: 'same-origin' });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const data = await response.json();
      items = Array.isArray(data) ? data : [];
      fuse = new Fuse(items, {
        keys: [
          { name: 'title', weight: 3 },
          { name: 'description', weight: 2 },
          { name: 'keywords', weight: 2 },
          { name: 'headings.title', weight: 1.5 },
          { name: 'section', weight: 1 },
          { name: 'summary', weight: 1 }
        ],
        includeScore: true,
        includeMatches: true,
        ignoreLocation: true,
        threshold: 0.35,
        minMatchCharLength: 2
      });
      hasLoaded = true;
      run(input.value);
    } catch (error) {
      console.error('Search index load error:', error);
      if (statusEl) statusEl.textContent = '';
      resultsEl.innerHTML = '<div class="search-empty">Could not load the search index. Please try again.</div>';
    } finally {
      loading = false;
    }
  };

  // Initialise from the URL so searches are shareable/linkable.
  const initialQuery = getParam('q');
  const initialVersion = getParam('version');
  if (initialVersion && versionSelect) versionSelect.value = initialVersion;
  input.value = initialQuery;
  lastState = { q: initialQuery, version: initialVersion };
  loadIndex();

  input.addEventListener('input', (event) => {
    debouncedRun(event.target.value);
  });
  if (versionSelect) {
    versionSelect.addEventListener('change', () => {
      run(input.value);
      input.focus();
    });
  }
  if (form) {
    form.addEventListener('submit', (event) => {
      event.preventDefault();
      run(input.value);
    });
  }
})();
