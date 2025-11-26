(() => {
  const input = document.querySelector('[data-search-input]');
  const resultsContainer = document.querySelector('[data-search-results]');
  if (!input || !resultsContainer) return;

  const MIN_QUERY_LENGTH = 2;
  let index = [];
  let loading = false;
  let hasLoaded = false;
  const indexUrl = input.dataset.searchIndex || 'index.json';

  const renderResults = (matches) => {
    resultsContainer.innerHTML = '';

    if (!matches.length) {
      resultsContainer.dataset.visible = 'false';
      return;
    }

    matches.slice(0, 8).forEach((match) => {
      const item = document.createElement('a');
      item.className = 'sidebar__result';
      item.href = match.permalink;
      item.setAttribute('role', 'option');
      item.innerHTML = `<strong>${match.title}</strong><span>${match.description}</span>`;
      resultsContainer.appendChild(item);
    });

    resultsContainer.dataset.visible = 'true';
  };

  const normalize = (value) => value.toLowerCase();

  const search = (query) => {
    const q = normalize(query.trim());
    if (q.length < MIN_QUERY_LENGTH) {
      resultsContainer.dataset.visible = 'false';
      resultsContainer.innerHTML = '';
      return;
    }

    const matches = index.filter((entry) => {
      const text = `${entry.title} ${entry.description} ${entry.content}`;
      return normalize(text).includes(q);
    });

    renderResults(matches);
  };

  const loadIndex = async () => {
    if (hasLoaded || loading) return;
    loading = true;
    try {
      const response = await fetch(indexUrl, { credentials: 'same-origin' });
      if (!response.ok) throw new Error('Failed to fetch search index');
      index = await response.json();
      hasLoaded = true;
    } catch (error) {
      console.error(error);
    } finally {
      loading = false;
    }
  };

  input.addEventListener('input', (event) => {
    loadIndex();
    search(event.target.value);
  });

  input.addEventListener('focus', () => {
    loadIndex();
    if (input.value.length) {
      search(input.value);
    }
  });

  document.addEventListener('click', (event) => {
    if (event.target === input || resultsContainer.contains(event.target)) return;
    resultsContainer.dataset.visible = 'false';
  });
})();
