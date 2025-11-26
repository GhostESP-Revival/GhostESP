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
