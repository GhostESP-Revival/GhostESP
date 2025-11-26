document.addEventListener('DOMContentLoaded', function() {
  const sidebar = document.querySelector('[data-sidebar]');
  const toggles = document.querySelectorAll('[data-sidebar-toggle]');
  const overlay = document.querySelector('[data-sidebar-overlay]');
  const groups = document.querySelectorAll('[data-group-toggle]');
  
  if (!sidebar || !toggles.length) return;

  const DESKTOP_BREAKPOINT = 961;
  const isMobile = () => window.innerWidth < DESKTOP_BREAKPOINT;

  function toggleSidebar() {
    const isOpen = sidebar.classList.contains('sidebar--open');
    
    if (isOpen) {
      sidebar.classList.remove('sidebar--open');
      if (overlay) overlay.classList.remove('overlay--visible');
    } else {
      sidebar.classList.add('sidebar--open');
      if (overlay && isMobile()) overlay.classList.add('overlay--visible');
    }

    if (!isMobile()) {
      const state = sidebar.classList.contains('sidebar--open') ? 'open' : 'closed';
      localStorage.setItem('ghostesp-sidebar', state);
    }
  }

  toggles.forEach(toggle => {
    toggle.addEventListener('click', toggleSidebar);
  });

  if (overlay) {
    overlay.addEventListener('click', toggleSidebar);
  }

  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && sidebar.classList.contains('sidebar--open') && isMobile()) {
      toggleSidebar();
    }
  });

  window.addEventListener('resize', () => {
    if (!isMobile()) {
      if (overlay) overlay.classList.remove('overlay--visible');
    }
  });

  if (!isMobile()) {
    const stored = localStorage.getItem('ghostesp-sidebar');
    if (stored !== 'closed') {
      sidebar.classList.add('sidebar--open');
    }
  }

  groups.forEach(button => {
    button.addEventListener('click', (e) => {
      e.preventDefault();
      const isOpen = button.getAttribute('aria-expanded') === 'true';
      button.setAttribute('aria-expanded', String(!isOpen));
      
      const container = button.closest('[data-group]');
      if (container) {
        container.classList.toggle('group--open');
      }
      
      const items = container?.querySelector('[data-group-items]');
      if (items) {
        items.classList.toggle('group-items--open');
      }
    });
  });
});
