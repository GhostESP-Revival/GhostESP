document.addEventListener('DOMContentLoaded', function() {
  const controlsBlocks = document.querySelectorAll('[data-compat-controls]');

  controlsBlocks.forEach((controls) => {
    const table = document.querySelector(controls.dataset.table || '');
    if (!table || !table.tBodies.length) return;

    const searchInput = controls.querySelector('[data-compat-search]');
    const featureInputs = Array.from(controls.querySelectorAll('[data-compat-feature]'));
    const count = controls.querySelector('[data-compat-count]');
    const clearButton = controls.querySelector('[data-compat-clear]');
    const empty = document.querySelector('[data-compat-empty]');
    const rows = Array.from(table.tBodies[0].rows);
    const columns = new Map(
      Array.from(table.tHead.rows[0].cells).map((cell, index) => [cell.textContent.trim(), index])
    );

    function hasSupport(cell) {
      if (!cell) return false;
      const value = cell.textContent.trim().toLowerCase();
      return value !== '' && value !== '✗' && value !== 'manual';
    }

    function update() {
      const query = searchInput ? searchInput.value.trim().toLowerCase() : '';
      const requiredFeatures = featureInputs
        .filter((input) => input.checked)
        .map((input) => input.dataset.compatFeature);
      let visible = 0;

      rows.forEach((row) => {
        const rowText = row.textContent.toLowerCase();
        const matchesSearch = !query || rowText.includes(query);
        const matchesFeatures = requiredFeatures.every((feature) => {
          const index = columns.get(feature);
          return Number.isInteger(index) && hasSupport(row.cells[index]);
        });
        const show = matchesSearch && matchesFeatures;

        row.hidden = !show;
        if (show) visible += 1;
      });

      if (count) {
        count.textContent = `${visible} of ${rows.length} boards shown`;
      }
      if (empty) {
        empty.hidden = visible !== 0;
      }
    }

    function clear() {
      if (searchInput) searchInput.value = '';
      featureInputs.forEach((input) => {
        input.checked = false;
      });
      update();
    }

    if (searchInput) searchInput.addEventListener('input', update);
    featureInputs.forEach((input) => input.addEventListener('change', update));
    if (clearButton) clearButton.addEventListener('click', clear);
    update();
  });
});
