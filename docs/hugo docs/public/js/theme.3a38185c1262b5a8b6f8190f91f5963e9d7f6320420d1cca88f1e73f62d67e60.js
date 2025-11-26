document.addEventListener('DOMContentLoaded', function() {
  const toggle = document.querySelector('[data-theme-toggle]');
  const icon = document.querySelector('.theme-toggle-icon');
  const root = document.documentElement;

  if (!toggle) return;

  const themes = ['light', 'dark'];

  function setTheme(theme) {
    root.setAttribute('data-theme', theme);
    if (icon) {
      icon.classList.toggle('is-dark', theme === 'dark');
      icon.setAttribute('data-theme-icon', theme);
      icon.style.transform = theme === 'dark' ? 'rotate(40deg)' : 'rotate(0deg)';
      requestAnimationFrame(() => {
        icon.style.transform = theme === 'dark' ? 'rotate(0deg)' : 'rotate(-40deg)';
      });
    }
    toggle.setAttribute('aria-pressed', theme === 'dark');
  }

  function toggleTheme() {
    const current = root.getAttribute('data-theme') || 'light';
    const nextTheme = current === 'dark' ? 'light' : 'dark';
    localStorage.setItem('ghostesp-theme', nextTheme);
    setTheme(nextTheme);
  }

  toggle.addEventListener('click', toggleTheme);

  const stored = localStorage.getItem('ghostesp-theme');
  const initialTheme = stored && themes.includes(stored)
    ? stored
    : (root.getAttribute('data-theme') || themes[0]);

  setTheme(initialTheme);
});
