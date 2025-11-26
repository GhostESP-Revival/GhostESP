document.addEventListener('DOMContentLoaded', function() {
  const toggle = document.querySelector('[data-theme-toggle]');
  const icon = document.querySelector('[data-theme-toggle-icon]');
  const root = document.documentElement;

  if (!toggle) return;

  const themes = ['light', 'dark'];
  const icons = ['🌙', '☀️'];

  function setTheme(theme) {
    root.setAttribute('data-theme', theme);
    if (icon) {
      icon.classList.add('rotating');
      setTimeout(() => {
        icon.textContent = theme === 'dark' ? icons[1] : icons[0];
        icon.classList.remove('rotating');
      }, 200);
    }
  }

  function toggleTheme() {
    const current = root.getAttribute('data-theme') || 'light';
    const nextTheme = current === 'dark' ? 'light' : 'dark';
    localStorage.setItem('ghostesp-theme', nextTheme);
    setTheme(nextTheme);
  }

  toggle.addEventListener('click', toggleTheme);

  const stored = localStorage.getItem('ghostesp-theme');
  if (stored && themes.includes(stored)) {
    setTheme(stored);
  } else {
    const current = root.getAttribute('data-theme') || 'light';
    if (icon) {
      icon.textContent = current === 'dark' ? icons[1] : icons[0];
    }
  }
});
