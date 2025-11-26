(() => {
  const input = document.querySelector('[data-search-input]');
  const resultsContainer = document.querySelector('[data-search-results]');
  if (!input || !resultsContainer) return;

  const MIN_QUERY_LENGTH = 2;
  const MAX_RESULTS = 8;
  let fuse = null;
  let loading = false;
  let hasLoaded = false;
  let selectedIndex = -1;
  let debounceTimer = null;
  const indexUrl = input.dataset.searchIndex || 'index.json';

  const escapeHtml = (text) => {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
  };

  const highlightMatches = (text, query) => {
    if (!query) return escapeHtml(text);
    const regex = new RegExp(`(${query.split(' ').map(t => t.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')).join('|')})`, 'gi');
    return escapeHtml(text).replace(regex, '<mark>$1</mark>');
  };

  const buildResultHref = (result) => {
    const permalink = result.item.permalink;
    const matches = result.matches || [];
    const contentMatch = matches.find(match => match.key === 'content') || matches[0];
    if (!contentMatch || !contentMatch.indices || !contentMatch.indices.length || !contentMatch.value) {
      return permalink;
    }

    const [start, end] = contentMatch.indices[0];
    const matchedText = contentMatch.value.slice(start, end + 1).trim();
    if (!matchedText) {
      return permalink;
    }

    const fragment = encodeURIComponent(matchedText.replace(/\s+/g, ' '));
    return `${permalink}#:~:text=${fragment}`;
  };

  const updateSelection = () => {
    const items = resultsContainer.querySelectorAll('.sidebar__result');
    items.forEach((item, idx) => {
      item.classList.toggle('sidebar__result--active', idx === selectedIndex);
    });
  };

  const renderResults = (results, query) => {
    resultsContainer.innerHTML = '';

    if (!results.length) {
      resultsContainer.innerHTML = '<div class="sidebar__result--empty">No results found</div>';
      resultsContainer.dataset.visible = 'true';
      return;
    }

    const count = document.createElement('div');
    count.className = 'sidebar__result--count';
    count.textContent = `${results.length} result${results.length !== 1 ? 's' : ''}`;
    resultsContainer.appendChild(count);

    results.slice(0, MAX_RESULTS).forEach((result, idx) => {
      const match = result.item;
      const item = document.createElement('a');
      item.className = 'sidebar__result';
      item.href = buildResultHref(result);
      item.setAttribute('role', 'option');
      item.dataset.index = idx;
      
      const title = highlightMatches(match.title, query);
      const description = match.description ? highlightMatches(match.description.substring(0, 100), query) : '';
      
      item.innerHTML = `<strong>${title}</strong>${description ? `<span>${description}...</span>` : ''}`;
      resultsContainer.appendChild(item);
    });

    selectedIndex = -1;
    resultsContainer.dataset.visible = 'true';
  };

  const search = (query) => {
    const q = query.trim();
    if (q.length < MIN_QUERY_LENGTH) {
      resultsContainer.dataset.visible = 'false';
      resultsContainer.innerHTML = '';
      return;
    }

    if (!fuse) {
      resultsContainer.innerHTML = '<div class="sidebar__result--loading">Loading search index...</div>';
      resultsContainer.dataset.visible = 'true';
      return;
    }

    const results = fuse.search(q);
    renderResults(results, q);
  };

  const debouncedSearch = (query) => {
    clearTimeout(debounceTimer);
    debounceTimer = setTimeout(() => search(query), 300);
  };

  const loadIndex = async () => {
    if (hasLoaded || loading) return;
    loading = true;
    try {
      const response = await fetch(indexUrl, { credentials: 'same-origin' });
      if (!response.ok) throw new Error('Failed to fetch search index');
      const data = await response.json();
      
      fuse = new Fuse(data, {
        keys: [
          { name: 'title', weight: 2 },
          { name: 'description', weight: 1.5 },
          { name: 'content', weight: 1 }
        ],
        includeScore: true,
        includeMatches: true,
        threshold: 0.4,
        minMatchCharLength: 2,
        ignoreLocation: true
      });
      
      hasLoaded = true;
      if (input.value.length >= MIN_QUERY_LENGTH) {
        search(input.value);
      }
    } catch (error) {
      console.error('Search index load error:', error);
      resultsContainer.innerHTML = '<div class="sidebar__result--error">Failed to load search</div>';
    } finally {
      loading = false;
    }
  };

  input.addEventListener('input', (event) => {
    loadIndex();
    debouncedSearch(event.target.value);
  });

  input.addEventListener('focus', () => {
    loadIndex();
    if (input.value.length >= MIN_QUERY_LENGTH) {
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
      resultsContainer.dataset.visible = 'false';
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
    resultsContainer.dataset.visible = 'false';
  });
})();
