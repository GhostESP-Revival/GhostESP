document.addEventListener('DOMContentLoaded', function() {
  const sidebar = document.getElementById('sidebar');
  const overlay = document.getElementById('overlay');
  const preInitStyle = document.getElementById('sidebar-pre-init');
  const toggleButton = document.querySelector('.topbar__toggle');
  const groupToggles = sidebar ? sidebar.querySelectorAll('.sidebar__group-toggle') : [];

  if (window.innerWidth >= 992) {
    if (localStorage.getItem('sidebarState') === 'open') {
      if (sidebar) sidebar.classList.add('show');
    }
    if (preInitStyle) {
      preInitStyle.remove();
    }
  }

  if (sidebar && toggleButton) {
    const isOpen = sidebar.classList.contains('show');
    toggleButton.classList.toggle('is-open', isOpen);
    toggleButton.setAttribute('aria-expanded', isOpen ? 'true' : 'false');
  }

  groupToggles.forEach((toggle) => {
    const targetSelector = toggle.getAttribute('data-bs-target');
    if (!targetSelector) return;
    const target = document.querySelector(targetSelector);
    if (!target) return;
    const parent = toggle.closest('.sidebar__group');

    target.addEventListener('show.bs.collapse', () => {
      toggle.classList.add('sidebar__group-toggle--active');
      toggle.classList.remove('collapsed');
      toggle.setAttribute('aria-expanded', 'true');
      if (parent) parent.classList.add('group--open');
    });

    target.addEventListener('hide.bs.collapse', () => {
      toggle.classList.remove('sidebar__group-toggle--active');
      toggle.classList.add('collapsed');
      toggle.setAttribute('aria-expanded', 'false');
      if (parent) parent.classList.remove('group--open');
    });
  });

  const versionSelect = document.querySelector('[data-version-select]');
  if (versionSelect) {
    const sidebarEl = document.getElementById('sidebar');
    const storedVersion = localStorage.getItem('ghostesp-doc-version');
    const validVersions = Array.from(versionSelect.options).map((option) => option.value);
    const currentPath = window.location.pathname;
    const match = currentPath.match(/^\/([^\/]+)(\/.*)?$/);
    const pathVersion = match && validVersions.includes(match[1]) ? match[1] : null;

    if (storedVersion && validVersions.includes(storedVersion) && storedVersion !== pathVersion) {
      const suffix = match && match[2] ? match[2] : '/';
      const targetPath = `/${storedVersion}${suffix}`.replace(/\/+$/, '/');
      localStorage.setItem('ghostesp-doc-version', storedVersion);
      window.location.replace(targetPath);
      return;
    }

    if (pathVersion) {
      versionSelect.value = pathVersion;
    } else if (sidebarEl && sidebarEl.dataset.currentVersion) {
      versionSelect.value = sidebarEl.dataset.currentVersion;
    }

    versionSelect.addEventListener('change', () => {
      const newVersion = versionSelect.value;
      if (!newVersion || !validVersions.includes(newVersion)) return;
      const current = window.location.pathname.match(/^\/([^\/]+)(\/.*)?$/);
      const suffix = current && validVersions.includes(current[1]) && current[2] ? current[2] : '/';
      const destination = `/${newVersion}${suffix}`.replace(/\/+$/, '/');
      localStorage.setItem('ghostesp-doc-version', newVersion);
      window.location.href = destination;
    });
  }
});

function toggleSidebar() {
  const isMobile = window.innerWidth <= 991.98;
  const sidebar = document.getElementById('sidebar');
  const overlay = document.getElementById('overlay');
  const toggleButton = document.querySelector('.topbar__toggle');

  if (!sidebar) return;
  
  sidebar.classList.toggle('show');
  
  if (isMobile) {
    if (overlay) overlay.classList.toggle('show');
  } else {
    const sidebarState = sidebar.classList.contains('show') ? 'open' : 'closed';
    localStorage.setItem('sidebarState', sidebarState);
  }

  if (toggleButton) {
    const isOpen = sidebar.classList.contains('show');
    toggleButton.classList.toggle('is-open', isOpen);
    toggleButton.setAttribute('aria-expanded', isOpen ? 'true' : 'false');
  }
}
