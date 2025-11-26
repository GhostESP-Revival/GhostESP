document.addEventListener('DOMContentLoaded', function() {
  const sidebar = document.getElementById('sidebar');
  const overlay = document.getElementById('overlay');
  const preInitStyle = document.getElementById('sidebar-pre-init');
  
  if (window.innerWidth >= 992) {
    if (localStorage.getItem('sidebarState') === 'open') {
      if (sidebar) sidebar.classList.add('show');
    }
    if (preInitStyle) {
      preInitStyle.remove();
    }
  }
});

function toggleSidebar() {
  const isMobile = window.innerWidth <= 991.98;
  const sidebar = document.getElementById('sidebar');
  const overlay = document.getElementById('overlay');
  
  if (!sidebar) return;
  
  sidebar.classList.toggle('show');
  
  if (isMobile) {
    if (overlay) overlay.classList.toggle('show');
  } else {
    const sidebarState = sidebar.classList.contains('show') ? 'open' : 'closed';
    localStorage.setItem('sidebarState', sidebarState);
  }
}
