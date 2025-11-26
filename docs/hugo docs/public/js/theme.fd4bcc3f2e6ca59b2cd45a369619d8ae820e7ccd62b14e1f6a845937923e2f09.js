document.addEventListener('DOMContentLoaded', function() {
  const toggle = document.querySelector('[data-theme-toggle]');
  const icon = document.querySelector('[data-theme-toggle-icon]');
  const root = document.documentElement;
  
  if (!toggle) return;

  function setTheme(theme) {
    root.setAttribute('data-theme', theme);
    if (icon) {
      icon.textContent = theme === 'dark' ? '☀️' : '🌙';
    }
  }

  function toggleTheme() {
    const current = root.getAttribute('data-theme') || 'light';
    const next = current === 'dark' ? 'light' : 'dark';
    localStorage.setItem('ghostesp-theme', next);
    setTheme(next);
  }

  toggle.addEventListener('click', toggleTheme);

  const stored = localStorage.getItem('ghostesp-theme');
  if (stored) {
    setTheme(stored);
  }
});
