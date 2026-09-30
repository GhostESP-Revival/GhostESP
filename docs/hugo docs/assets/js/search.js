(() => {
  const input = document.querySelector('[data-search-input]');
  const resultsContainer = document.querySelector('[data-search-results]');
  if (!input || !resultsContainer) return;

  const MIN_QUERY_LENGTH = 2;
  const MAX_RESULTS = 8;
  const MAX_HEADING_LINKS = 3;
  const DEBOUNCE_MS = 150;

  const moduleUrl = input.dataset.pagefindModule || '/pagefind/pagefind.js';
  const searchPageUrl = input.dataset.searchPageUrl || 'search/';
  const versionEl = document.querySelector('[data-current-version]');
  const currentVersion = versionEl ? versionEl.dataset.currentVersion : null;

  let pagefind = null;
  let loading = false;
  let hasLoaded = false;
  let selectedIndex = -1;
  let debounceTimer = null;
  let requestId = 0;

  const escapeHtml = (text) => {
    const div = document.createElement('div');
    div.textContent = text == null ? '' : String(text);
    return div.innerHTML;
  };

  const cleanTitle = (text) => String(text == null ? '' : text)
    .replace(/\s*[·|]\s*GhostESP Documentation\s*$/i, '')
    .trim();

  const setVisible = (visible) => {
    resultsContainer.dataset.visible = visible ? 'true' : 'false';
  };

  const showMessage = (className, text) => {
    resultsContainer.innerHTML = '';
    const el = document.createElement('div');
    el.className = className;
    el.textContent = text;
    resultsContainer.appendChild(el);
    setVisible(true);
  };

  const appendSearchAllLink = (query, label) => {
    const trimmed = (query || '').trim();
    if (!trimmed || !searchPageUrl) return;
    const link = document.createElement('a');
    link.className = 'sidebar__result-all';
    const params = new URLSearchParams({ q: trimmed });
    if (currentVersion) params.set('version', currentVersion);
    link.href = `${searchPageUrl}?${params.toString()}`;
    link.textContent = label || `Search all versions for “${trimmed}”`;
    resultsContainer.appendChild(link);
  };

  const headingLinks = (data) => {
    const subs = Array.isArray(data.sub_results) ? data.sub_results : [];
    const seen = new Set([data.url]);
    const links = [];
    for (const sub of subs) {
      if (!sub || !sub.url || seen.has(sub.url)) continue;
      if (!sub.title) continue;
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

  const renderResults = (items, query, options) => {
    const opts = options || {};
    resultsContainer.innerHTML = '';

    if (!items.length) {
      showMessage('sidebar__result--empty', currentVersion && !opts.fellBack
        ? 'No results found in this version'
        : 'No results found');
      appendSearchAllLink(query);
      return;
    }

    const total = opts.total || items.length;
    const count = document.createElement('div');
    count.className = 'sidebar__result--count';
    count.textContent = total > items.length
      ? `Top ${items.length} of ${total} results${opts.fellBack ? ' in all versions' : ''}`
      : `${total} result${total !== 1 ? 's' : ''}${opts.fellBack ? ' in all versions' : ''}`;
    resultsContainer.appendChild(count);

    items.forEach((data, idx) => {
      const item = document.createElement('a');
      item.className = 'sidebar__result';
      item.href = matchedSectionUrl(data) || data.url;
      item.setAttribute('role', 'option');
      item.dataset.index = idx;

      const title = escapeHtml(cleanTitle(data.meta && data.meta.title) || data.url);
      const excerpt = data.excerpt || '';
      const subs = headingLinks(data);
      const subsHtml = subs.length
        ? `<span class="sidebar__result-headings">${subs
            .map((sub) => `<span class="sidebar__result-heading">${escapeHtml(cleanTitle(sub.title))}</span>`)
            .join('')}</span>`
        : '';

      item.innerHTML = `<strong>${title}</strong>${excerpt ? `<span>${excerpt}</span>` : ''}${subsHtml}`;
      resultsContainer.appendChild(item);
    });

    appendSearchAllLink(query);
    selectedIndex = -1;
    setVisible(true);
  };

  const search = async (query) => {
    const q = (query || '').trim();
    if (q.length < MIN_QUERY_LENGTH) {
      setVisible(false);
      resultsContainer.innerHTML = '';
      return;
    }

    if (!hasLoaded) {
      loadPagefind();
      return;
    }
    if (!pagefind) return;

    const id = ++requestId;

    let response;
    try {
      response = await pagefind.search(q, currentVersion ? { filters: { version: currentVersion } } : undefined);
    } catch (error) {
      console.error('Pagefind search error:', error);
      if (id === requestId) showMessage('sidebar__result--error', 'Search failed');
      return;
    }
    if (id !== requestId) return;

    let refs = response.results || [];
    let fellBack = false;

    // If the current firmware version has no match, fall back to every version
    // rather than showing an empty result list.
    if (!refs.length && currentVersion) {
      try {
        const allVersions = await pagefind.search(q);
        if (id !== requestId) return;
        refs = allVersions.results || [];
        fellBack = refs.length > 0;
      } catch (error) {
        // Keep the empty state from the scoped search.
      }
    }

    const top = refs.slice(0, MAX_RESULTS);
    const items = await Promise.all(top.map((ref) => ref.data().catch(() => null)));
    if (id !== requestId) return;

    renderResults(items.filter(Boolean), q, { total: refs.length, fellBack });
  };

  const debouncedSearch = (query) => {
    clearTimeout(debounceTimer);
    debounceTimer = setTimeout(() => search(query), DEBOUNCE_MS);
  };

  const loadPagefind = async () => {
    if (hasLoaded || loading) return;
    loading = true;
    showMessage('sidebar__result--loading', 'Loading search…');
    try {
      pagefind = await import(moduleUrl);
      // Ask Pagefind to stamp result URLs with ?highlight=<query> so the page
      // they land on can mark the matched terms.
      await pagefind.options({ highlightParam: 'highlight' });
      hasLoaded = true;
      if (input.value.trim().length >= MIN_QUERY_LENGTH) {
        search(input.value);
      } else {
        setVisible(false);
      }
    } catch (error) {
      console.error('Pagefind load error:', error);
      showMessage('sidebar__result--error', 'Failed to load search');
    } finally {
      loading = false;
    }
  };

  const updateSelection = () => {
    const items = resultsContainer.querySelectorAll('.sidebar__result');
    items.forEach((item, idx) => {
      item.classList.toggle('sidebar__result--active', idx === selectedIndex);
    });
  };

  input.addEventListener('input', (event) => {
    loadPagefind();
    debouncedSearch(event.target.value);
  });

  input.addEventListener('focus', () => {
    loadPagefind();
    if (input.value.trim().length >= MIN_QUERY_LENGTH) {
      search(input.value);
    }
  });

  input.addEventListener('keydown', (event) => {
    const items = resultsContainer.querySelectorAll('.sidebar__result[href]');
    if (!items.length) return;

    if (event.key === 'ArrowDown') {
      event.preventDefault();
      selectedIndex = Math.min(selectedIndex + 1, items.length - 1);
      updateSelection();
      items[selectedIndex]?.scrollIntoView({ block: 'nearest' });
    } else if (event.key === 'ArrowUp') {
      event.preventDefault();
      selectedIndex = Math.max(selectedIndex - 1, -1);
      updateSelection();
      if (selectedIndex >= 0) {
        items[selectedIndex]?.scrollIntoView({ block: 'nearest' });
      }
    } else if (event.key === 'Enter' && selectedIndex >= 0) {
      event.preventDefault();
      items[selectedIndex]?.click();
    } else if (event.key === 'Escape') {
      event.preventDefault();
      setVisible(false);
      input.blur();
    }
  });

  document.addEventListener('keydown', (event) => {
    if ((event.ctrlKey || event.metaKey) && event.key === 'k') {
      event.preventDefault();
      input.focus();
      input.select();
    }
  });

  document.addEventListener('click', (event) => {
    if (event.target === input || resultsContainer.contains(event.target)) return;
    setVisible(false);
  });
})();
