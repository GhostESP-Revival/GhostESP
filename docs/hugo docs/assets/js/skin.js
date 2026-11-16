// Opt-in "neon" skin toggle.
// Adds/removes <html data-skin="neon">, persists the choice, and lazily loads
// the Sora webfont only while the skin is active. The initial state is applied
// by an inline bootstrap script in <head> to avoid a flash; this file handles
// user interaction and keeps the button state in sync.

document.addEventListener('DOMContentLoaded', function () {
  const root = document.documentElement;
  const toggle = document.querySelector('[data-skin-toggle]');
  const FONT_ID = 'ghostesp-skin-font';
  const FONT_HREF = 'https://fonts.googleapis.com/css2?family=Sora:wght@400;500;600;700&display=swap';

  function ensureFont(on) {
    const existing = document.getElementById(FONT_ID);
    if (on && !existing) {
      const link = document.createElement('link');
      link.id = FONT_ID;
      link.rel = 'stylesheet';
      link.href = FONT_HREF;
      document.head.appendChild(link);
    } else if (!on && existing) {
      existing.remove();
    }
  }

  function isOn() {
    return root.getAttribute('data-skin') === 'neon';
  }

  function setSkin(on) {
    if (on) {
      root.setAttribute('data-skin', 'neon');
      // Neon is always dark; restore the reader's own theme when it turns off.
      root.setAttribute('data-theme', 'dark');
    } else {
      root.removeAttribute('data-skin');
      let theme = 'light';
      try {
        theme = localStorage.getItem('ghostesp-theme') ||
          (window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light');
      } catch (e) {}
      root.setAttribute('data-theme', theme);
    }
    try {
      localStorage.setItem('ghostesp-skin', on ? 'neon' : 'default');
    } catch (e) {}
    ensureFont(on);
    if (toggle) {
      toggle.setAttribute('aria-pressed', on ? 'true' : 'false');
      toggle.setAttribute('aria-label', on ? 'Switch to original style' : 'Try the new style');
    }
  }

  if (!toggle) return;

  setSkin(isOn());

  toggle.addEventListener('click', function () {
    setSkin(!isOn());
  });
});
