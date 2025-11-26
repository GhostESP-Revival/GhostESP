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
    const escaped = query.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    const regex = new RegExp(`(${escaped})`, 'gi');
    return escapeHtml(text).replace(regex, '<mark>$1</mark>');
  };

  const extractMatchFragment = (result, query) => {
    const content = result.item.content || '';
    const trimmedQuery = query.trim();
    if (!content || !trimmedQuery) {
      return null;
    }

    const lowerQuery = trimmedQuery.toLowerCase();
    const lowerContent = content.toLowerCase();
    let index = lowerContent.indexOf(lowerQuery);

    if (index === -1) {
      const matches = result.matches || [];
      const fallback = matches.find(match => match.key === 'content' && match.value);
      if (fallback) {
        const value = fallback.value;
        const lowerValue = value.toLowerCase();
        const matchIndex = lowerValue.indexOf(lowerQuery);
        if (matchIndex !== -1) {
          const segment = value.slice(Math.max(0, matchIndex - 60), Math.min(value.length, matchIndex + trimmedQuery.length + 60));
          const snippet = segment.replace(/\s+/g, ' ').trim();
          if (snippet) {
            return {
              snippet,
              leading: matchIndex > 0,
              trailing: matchIndex + trimmedQuery.length < value.length
            };
          }
        }
      }
      return null;
    }

    const totalLength = content.length;
    const start = Math.max(0, index - 60);
    const end = Math.min(totalLength, index + trimmedQuery.length + 60);
    const snippet = content.slice(start, end).replace(/\s+/g, ' ').trim();
    if (!snippet) {
      return null;
    }

    return {
      snippet,
      leading: start > 0,
      trailing: end < totalLength
    };
  };

  const buildMatchSnippet = (result, query) => {
    const fragment = extractMatchFragment(result, query);
    if (fragment) {
      let text = fragment.snippet;
      if (fragment.leading) {
        text = `…${text}`;
      }
      if (fragment.trailing) {
        text = `${text}…`;
      }
      return highlightMatches(text, query);
    }

    const description = result.item.description || '';
    if (description) {
      return highlightMatches(description.substring(0, 120), query);
    }

    return '';
  };

  const buildResultHref = (result, query) => {
    const permalink = result.item.permalink;
    const fragment = extractMatchFragment(result, query);
    if (!fragment) {
      return permalink;
    }

    const fragmentText = fragment.snippet.replace(/\s+/g, ' ').trim();
    if (!fragmentText) {
      return permalink;
    }

    const encoded = encodeURIComponent(fragmentText);
    return `${permalink}#:~:text=${encoded}`;
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
      item.href = buildResultHref(result, query);
      item.setAttribute('role', 'option');
      item.dataset.index = idx;
      
      const title = highlightMatches(match.title, query);
      const snippet = buildMatchSnippet(result, query);
      
      item.innerHTML = `<strong>${title}</strong>${snippet ? `<span>${snippet}</span>` : ''}`;
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

    const phrase = q.toLowerCase();
    const results = fuse.search(q);
    let filteredResults = results.filter(result => {
      const item = result.item;
      return [item.content, item.description, item.title].some(field => field && field.toLowerCase().includes(phrase));
    });

    if (!filteredResults.length) {
      filteredResults = results;
    }

    renderResults(filteredResults, q);
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
