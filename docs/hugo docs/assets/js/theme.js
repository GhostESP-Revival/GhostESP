document.addEventListener('DOMContentLoaded', function() {
  const toggle = document.querySelector('[data-theme-toggle]');
  const icon = document.querySelector('[data-theme-icon]');
  const root = document.documentElement;

  if (!toggle) return;

  const themes = ['light', 'dark'];

  if (icon) {
    icon.addEventListener('animationend', () => {
      icon.classList.remove('icon-theme--spin');
    });
  }

  function setTheme(theme, options = {}) {
    root.setAttribute('data-theme', theme);
    localStorage.setItem('ghostesp-theme', theme);

    if (icon && options.animate) {
      icon.classList.remove('icon-theme--spin');
      void icon.offsetWidth;
      icon.classList.add('icon-theme--spin');
    }

    toggle.setAttribute('aria-pressed', theme === 'dark' ? 'true' : 'false');
  }

  function toggleTheme() {
    const current = root.getAttribute('data-theme') || 'light';
    const nextTheme = current === 'dark' ? 'light' : 'dark';
    setTheme(nextTheme, { animate: true });
  }

  toggle.addEventListener('click', toggleTheme);

  const stored = localStorage.getItem('ghostesp-theme');
  if (stored && themes.includes(stored)) {
    setTheme(stored);
  } else {
    const current = root.getAttribute('data-theme') || 'light';
    toggle.setAttribute('aria-pressed', current === 'dark' ? 'true' : 'false');
  }
});
