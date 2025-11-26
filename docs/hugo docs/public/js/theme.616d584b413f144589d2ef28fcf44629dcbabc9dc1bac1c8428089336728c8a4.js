document.addEventListener('DOMContentLoaded', function() {
  const toggle = document.querySelector('[data-theme-toggle]');
  const icon = document.querySelector('[data-theme-toggle-icon]');
  const root = document.documentElement;

  if (!toggle) return;

  const themes = ['light', 'dark', 'spooky'];
  const icons = ['🌙', '🦇', '☀️']; // moon for light, bat for dark, sun for spooky

  function getCurrentThemeIndex() {
    const current = root.getAttribute('data-theme') || 'light';
    return themes.indexOf(current);
  }

  function setTheme(theme) {
    root.setAttribute('data-theme', theme);
    const themeIndex = themes.indexOf(theme);
    if (icon) {
      icon.textContent = icons[themeIndex];
    }
  }

  function toggleTheme() {
    const currentIndex = getCurrentThemeIndex();
    const nextIndex = (currentIndex + 1) % themes.length;
    const nextTheme = themes[nextIndex];
    localStorage.setItem('ghostesp-theme', nextTheme);
    setTheme(nextTheme);
  }

  toggle.addEventListener('click', toggleTheme);

  const stored = localStorage.getItem('ghostesp-theme');
  if (stored && themes.includes(stored)) {
    setTheme(stored);
  }
});
