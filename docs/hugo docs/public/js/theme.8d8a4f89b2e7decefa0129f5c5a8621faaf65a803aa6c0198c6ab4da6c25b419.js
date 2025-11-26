(() => {
  const root = document.documentElement;
  const toggle = document.querySelector('[data-theme-toggle]');
  const icon = document.querySelector('[data-theme-toggle-icon]');
  if (!toggle || !icon) return;

  const THEMES = ['light', 'dark'];
  const storageKey = 'ghostesp-theme';

  const getStoredTheme = () => localStorage.getItem(storageKey);
  const systemPrefersDark = () => window.matchMedia('(prefers-color-scheme: dark)').matches;

  const setTheme = (theme, { persist } = { persist: true }) => {
    let applied = theme;
    if (theme === 'auto') {
      applied = systemPrefersDark() ? 'dark' : 'light';
    }
    root.setAttribute('data-theme', applied);
    icon.textContent = applied === 'dark' ? '☀️' : '🌙';
    root.classList.toggle('is-dark', applied === 'dark');
    if (persist) {
      localStorage.setItem(storageKey, theme);
    }
  };

  const initTheme = () => {
    const stored = getStoredTheme();
    if (stored && (THEMES.includes(stored) || stored === 'auto')) {
      setTheme(stored, { persist: false });
    } else {
      setTheme(root.getAttribute('data-theme') || 'auto', { persist: false });
    }
  };

  const rotateTheme = () => {
    const current = root.getAttribute('data-theme') || 'auto';
    let next;
    if (current === 'light') {
      next = 'dark';
    } else if (current === 'dark') {
      next = 'auto';
    } else {
      next = 'light';
    }
    setTheme(next);
  };

  toggle.addEventListener('click', rotateTheme);

  window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
    const stored = getStoredTheme();
    if (!stored || stored === 'auto') {
      setTheme('auto', { persist: false });
    }
  });

  initTheme();
})();
