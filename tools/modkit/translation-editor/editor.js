'use strict';
const token = location.hash.slice(1);
const $ = id => document.getElementById(id);
let catalog, revision, filename, dirty = false, limit = 80;
async function api(path, data) {
  const response = await fetch(path, {method: data ? 'POST' : 'GET', headers: {
    'X-Modkit-Token': token, ...(data ? {'Content-Type': 'application/json'} : {})},
    ...(data ? {body: JSON.stringify(data)} : {})});
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || 'Request failed');
  return result;
}
function status(text, error = false) { $('status').textContent = text; $('status').className = error ? 'error' : ''; }
function changed() { dirty = true; $('save').disabled = false; status('Unsaved changes'); }
function progress(value) {
  $('progress').value = value.percent;
  $('counts').textContent = `${value.translated} / ${value.total} translated · ${value.remaining} left · ${value.percent}%`;
  if (value.errors.length) status(`${value.errors.length} issue(s) to fix before building. ${value.errors[0].key || ''} ${value.errors[0].error}`, true);
}
function render() {
  const container = $('entries'); container.replaceChildren();
  const search = $('search').value.toLocaleLowerCase(), remaining = $('remaining').checked;
  const entries = Object.entries(catalog.entries).filter(([key, item]) =>
    (!remaining || item.translation === null) &&
    `${key}\n${item.source}\n${item.translation || ''}`.toLocaleLowerCase().includes(search));
  for (const [key, item] of entries.slice(0, limit)) {
    const row = document.createElement('section'); row.className = 'entry';
    const heading = document.createElement('div'); heading.className = 'key'; heading.textContent = key + (item.context ? ` · ${item.context}` : '');
    const source = document.createElement('p'); source.className = 'source'; source.textContent = item.source;
    const edit = document.createElement('div'), text = document.createElement('textarea');
    text.setAttribute('aria-label', `Translation for ${key}`);
    text.value = item.translation === null ? '' : item.translation; text.placeholder = item.source;
    const label = document.createElement('label'), useOriginal = document.createElement('input');
    useOriginal.type = 'checkbox'; useOriginal.checked = item.translation === null;
    // Typing starts a translation; the separate checkbox can restore fallback.
    text.addEventListener('input', () => { item.translation = text.value; useOriginal.checked = false; changed(); });
    useOriginal.addEventListener('change', () => { item.translation = useOriginal.checked ? null : text.value; changed(); });
    label.append(useOriginal, document.createTextNode(' Use original'));
    edit.append(text, label); row.append(heading, source, edit); container.append(row);
  }
  $('more').hidden = entries.length <= limit;
}
async function save() {
  if (!dirty) return;
  $('save').disabled = true;
  try {
    const result = await api('/api/catalog?file=' + encodeURIComponent(filename), {revision,
      translations: Object.fromEntries(Object.entries(catalog.entries).map(([key, value]) => [key, value.translation]))});
    revision = result.revision; dirty = false; status('Saved'); progress(result.progress);
  } catch (error) { $('save').disabled = false; status(error.message, true); throw error; }
}
async function open(name) {
  const result = await api('/api/catalog?file=' + encodeURIComponent(name));
  filename = name; catalog = result.catalog; revision = result.revision; dirty = false; limit = 80;
  $('save').disabled = true; render();
}
$('catalog').addEventListener('change', async () => {
  try { await save(); await open($('catalog').value); }
  catch (error) { $('catalog').value = filename; status(error.message, true); }
});
$('save').addEventListener('click', () => { save().catch(() => {}); });
for (const id of ['search', 'remaining']) $(id).addEventListener('input', () => { limit = 80; render(); });
$('more').addEventListener('click', () => { limit += 80; render(); });
$('project-search').addEventListener('submit', async event => {
  event.preventDefault();
  try {
    const result = await api('/api/search?q=' + encodeURIComponent($('global-search').value));
    const region = $('search-results'); region.replaceChildren();
    const count = document.createElement('p'); count.textContent = `${result.total} matches${result.total > 100 ? ' · showing the first 100; narrow the search for more' : ''}`; region.append(count);
    for (const item of result.matches) {
      const button = document.createElement('button'); button.type = 'button'; button.className = 'search-result';
      button.textContent = `${item.context || item.key} · ${item.source}`;
      button.addEventListener('click', async () => {
        try {
          await save(); $('catalog').value = item.file; $('search').value = item.key; $('remaining').checked = false;
          await open(item.file); $('entries').scrollIntoView({block:'center'});
          const text = $('entries').querySelector('textarea'); if (text) text.focus();
        } catch (error) { status(error.message, true); }
      });
      region.append(button);
    }
  } catch (error) { status(error.message, true); }
});
window.addEventListener('beforeunload', event => { if (dirty) { event.preventDefault(); event.returnValue = ''; } });
(async () => {
  try {
    const result = await api('/api/project');
    $('project').textContent = `${result.name} · ${result.project.locale}`; progress(result.progress);
    for (const item of result.project.catalogs) {
      const option = document.createElement('option'); option.value = item.file; option.textContent = item.label || item.file; $('catalog').append(option);
    }
    if (result.project.catalogs.length) await open(result.project.catalogs[0].file);
    else status('This project has no text catalogs.');
  } catch (error) { status(error.message, true); }
})();
