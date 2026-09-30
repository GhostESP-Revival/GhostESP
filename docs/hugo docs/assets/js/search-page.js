(() => {
  // Resolve the container from the results element rather than the bare
  // [data-search-page] attribute: the sidebar input also carries a
  // data-search-page-url attribute and appears earlier in the DOM, so a bare
  // attribute lookup would hand back the wrong element.
  const resultsEl = document.querySelector('[data-search-page-results]');
  const root = resultsEl ? resultsEl.closest('[data-search-page]') : null;
  if (!root || !resultsEl) return;

  const input = root.querySelector('[data-search-page-input]');
  const versionSelect = root.querySelector('[data-search-page-version]');
  const statusEl = root.querySelector('[data-search-page-status]');
  const form = root.querySelector('[data-search-page-form]');
  if (!input) return;

  const moduleUrl = root.dataset.pagefindModule || '/pagefind/pagefind.js';
  const MIN_QUERY_LENGTH = 2;
  const MAX_RESULTS = 40;
  const MAX_HEADING_LINKS = 4;
  const DEBOUNCE_MS = 150;

  let pagefind = null;
  let hasLoaded = false;
  let loading = false;
  let debounceTimer = null;
  let requestId = 0;
  let lastState = { q: '', version: '' };

  const escapeHtml = (text) => {
    const div = document.createElement('div');
    div.textContent = text == null ? '' : String(text);
    return div.innerHTML;
  };

  const cleanTitle = (text) => String(text == null ? '' : text)
    .replace(/\s*[·|]\s*GhostESP Documentation\s*$/i, '')
    .trim();

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

  const headingLinks = (data) => {
    const subs = Array.isArray(data.sub_results) ? data.sub_results : [];
    const seen = new Set([data.url]);
    const links = [];
    for (const sub of subs) {
      if (!sub || !sub.url || !sub.title || seen.has(sub.url)) continue;
      seen.add(sub.url);
      links.push(sub);
      if (links.length >= MAX_HEADING_LINKS) break;
    }
    return links;
  };

  // Pagefind lists sub_results in document order, so the matched section is the
  // first heading whose excerpt contains a <mark> hit. Linking there lands the
  // reader on the passage they searched for instead of the top of the page.
  const matchedSectionUrl = (data) => {
    const subs = Array.isArray(data.sub_results) ? data.sub_results : [];
    for (const sub of subs) {
      if (!sub || !sub.url || sub.url.indexOf('#') === -1) continue;
      if (sub.excerpt && sub.excerpt.indexOf('<mark') !== -1) return sub.url;
    }
    return '';
  };

  const versionOf = (data) => {
    const filters = data.filters || {};
    const value = filters.version;
    if (Array.isArray(value)) return value[0] || '';
    return value || '';
  };

  const renderResults = (items, query, total) => {
    if (!items.length) {
      resultsEl.innerHTML = '<div class="search-empty">No matching pages. Try a shorter or different query.</div>';
      return;
    }

    resultsEl.innerHTML = items.map((data) => {
      const subs = headingLinks(data);
      const headingHtml = subs.length
        ? `<div class="search-result__headings">${subs
            .map((sub) => `<a href="${escapeHtml(sub.url)}">${escapeHtml(cleanTitle(sub.title))}</a>`)
            .join('')}</div>`
        : '';
      const version = versionOf(data);
      const section = (data.meta && data.meta.section) || 'Docs';
      const textUrl = (data.meta && data.meta.text_url) || '';
      const textLink = textUrl
        ? `<a class="search-result__text" href="${escapeHtml(textUrl)}">plain text</a>`
        : '';
      const href = matchedSectionUrl(data) || data.url;
      return `
        <article class="search-result">
          <div class="search-result__top">
            <h3 class="search-result__title"><a href="${escapeHtml(href)}">${escapeHtml(cleanTitle(data.meta && data.meta.title) || data.url)}</a></h3>
            <span class="search-result__version">${escapeHtml(version)}</span>
          </div>
          <p class="search-result__crumb">${escapeHtml(section)}</p>
          <p class="search-result__snippet">${data.excerpt || ''}</p>
          ${headingHtml}
          ${textLink}
        </article>`;
    }).join('');
  };

  const run = async (query) => {
    const q = (query || '').trim();
    const version = versionSelect ? versionSelect.value : '';
    syncUrl(q, version);

    if (q.length < MIN_QUERY_LENGTH) {
      if (statusEl) statusEl.textContent = '';
      resultsEl.innerHTML = '<div class="search-empty">Start typing to search every page across all firmware versions.</div>';
      return;
    }

    if (!hasLoaded) {
      loadPagefind();
      return;
    }
    if (!pagefind) return;

    const id = ++requestId;
    if (statusEl) statusEl.textContent = 'Searching…';

    let response;
    try {
      response = await pagefind.search(q, version ? { filters: { version } } : undefined);
    } catch (error) {
      console.error('Pagefind search error:', error);
      if (id === requestId && statusEl) statusEl.textContent = '';
      if (id === requestId) resultsEl.innerHTML = '<div class="search-empty">Search failed. Please try again.</div>';
      return;
    }
    if (id !== requestId) return;

    const refs = response.results || [];
    const shown = Math.min(refs.length, MAX_RESULTS);
    const items = await Promise.all(refs.slice(0, MAX_RESULTS).map((ref) => ref.data().catch(() => null)));
    if (id !== requestId) return;

    if (statusEl) {
      statusEl.textContent = refs.length
        ? `${refs.length} result${refs.length === 1 ? '' : 's'}${refs.length > shown ? ` (showing first ${shown})` : ''}`
        : '';
    }
    renderResults(items.filter(Boolean), q, refs.length);
  };

  const debouncedRun = (query) => {
    clearTimeout(debounceTimer);
    debounceTimer = setTimeout(() => run(query), DEBOUNCE_MS);
  };

  const loadPagefind = async () => {
    if (hasLoaded || loading) return;
    loading = true;
    if (statusEl) statusEl.textContent = 'Loading search…';
    try {
      pagefind = await import(moduleUrl);
      // Ask Pagefind to stamp result URLs with ?highlight=<query> so the page
      // they land on can mark the matched terms.
      await pagefind.options({ highlightParam: 'highlight' });
      hasLoaded = true;
      run(input.value);
    } catch (error) {
      console.error('Pagefind load error:', error);
      if (statusEl) statusEl.textContent = '';
      resultsEl.innerHTML = '<div class="search-empty">Could not load search. Please try again.</div>';
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
  loadPagefind();

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
