document.addEventListener('DOMContentLoaded', function() {
  const sidebar = document.getElementById('sidebar');
  const overlay = document.getElementById('overlay');
  const preInitStyle = document.getElementById('sidebar-pre-init');
  const toggleButton = document.querySelector('.topbar__toggle');
  
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
