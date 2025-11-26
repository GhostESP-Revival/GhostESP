(() => {
  const sidebar = document.querySelector('[data-sidebar]');
  const toggles = document.querySelectorAll('[data-sidebar-toggle]');
  const overlay = document.querySelector('[data-sidebar-overlay]');
  const groups = [...document.querySelectorAll('[data-group-toggle]')];
  if (!sidebar || !toggles.length) return;

  const DESKTOP_QUERY = '(min-width: 961px)';
  const mq = window.matchMedia(DESKTOP_QUERY);

  const isDesktop = () => mq.matches;
  const setOverlay = (visible) => {
    if (!overlay) return;
    overlay.dataset.visible = visible ? 'true' : 'false';
  };

  const openSidebar = ({ persist } = { persist: true }) => {
    sidebar.setAttribute('data-open', 'true');
    setOverlay(!isDesktop());
    if (persist && isDesktop()) {
      localStorage.setItem('ghostesp-sidebar', 'open');
    }
  };

  const closeSidebar = ({ persist } = { persist: true }) => {
    sidebar.setAttribute('data-open', 'false');
    setOverlay(false);
    if (persist && isDesktop()) {
      localStorage.setItem('ghostesp-sidebar', 'closed');
    }
  };

  const toggleSidebar = () => {
    const isOpen = sidebar.getAttribute('data-open') === 'true';
    if (isOpen) {
      closeSidebar();
    } else {
      openSidebar();
    }
  };

  toggles.forEach((toggle) => {
    toggle.addEventListener('click', toggleSidebar);
  });

  if (overlay) {
    overlay.addEventListener('click', () => closeSidebar({ persist: false }));
  }

  document.addEventListener('keydown', (event) => {
    if (event.key === 'Escape' && sidebar.getAttribute('data-open') === 'true' && !isDesktop()) {
      closeSidebar({ persist: false });
    }
  });

  mq.addEventListener('change', (event) => {
    if (event.matches) {
      const stored = localStorage.getItem('ghostesp-sidebar');
      if (stored === 'closed') {
        closeSidebar({ persist: false });
      } else {
        openSidebar({ persist: false });
      }
    } else {
      closeSidebar({ persist: false });
    }
  });

  if (isDesktop()) {
    const stored = localStorage.getItem('ghostesp-sidebar');
    if (stored === 'closed') {
      closeSidebar({ persist: false });
    } else {
      openSidebar({ persist: false });
    }
  } else {
    closeSidebar({ persist: false });
  }

  const handleGroupToggle = (button) => {
    const open = button.getAttribute('data-open') === 'true';
    const nextState = !open;
    button.setAttribute('data-open', String(nextState));
    button.setAttribute('aria-expanded', String(nextState));
    const container = button.closest('[data-group]');
    if (container) {
      container.dataset.open = String(nextState);
    }
    const items = button.parentElement.querySelector('[data-group-items]');
    if (items) {
      items.dataset.open = String(nextState);
    }
  };

  groups.forEach((button) => {
    button.addEventListener('click', (event) => {
      event.preventDefault();
      handleGroupToggle(event.currentTarget);
    });
  });
})();
