// Товарный отчёт: себестоимость батона 31,20; 10.10 поступило 40, 12.10 продано 30, 13.10 списано 2.
// Отчёт за 12-13.10: начало 1 248,00, расход 936,00 + 62,40, конец 249,60; кнопка из «Остатков».
const post = async (path, body) => {
    const res = await fetch('/api/' + path, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body || {})});
    const data = await res.json();
    if (!res.ok) throw new Error(path + ': ' + data.error);
    return data;
};
const bread = (await (await fetch('/api/items')).json()).find(i => i.name === 'Батон').id;
const v1 = (await post('price-types', {name: 'Опт'})).id;
await post(`docs/${(await post('docs', {type: 'prices', date: '2026-10-01', price_type_id: 1, lines: [{item_id: bread, price_kop: 3120}]})).id}/post`);
await post(`docs/${(await post('docs', {type: 'production', date: '2026-10-10', lines: [{item_id: bread, qty: 40000}]})).id}/post`);
await post(`docs/${(await post('docs', {type: 'sale', date: '2026-10-12', price_type_id: v1, lines: [{item_id: bread, qty: 30000, price_kop: 4500}]})).id}/post`);
await post(`docs/${(await post('docs', {type: 'writeoff', date: '2026-10-13', comment: 'брак', lines: [{item_id: bread, qty: 2000}]})).id}/post`);

location.hash = '#/stock?date=2026-10-13';
await sleep(600);
btn('Товарный отчёт за период').click();
await sleep(600);
location.hash = '#/report?from=2026-10-12&to=2026-10-13';
await sleep(700);
const text = document.querySelector('.print-sheet').innerText.replace(/ /g, ' ');
for (const [what, re] of [['начало', /Остаток на 12\.10\.2026\s+1 248,00/], ['реализация', /Реализация\s+12\.10\.2026 00:00\s+1\s+936,00/],
    ['списание', /Списание\s+13\.10\.2026 00:00\s+1\s+62,40/], ['расход', /Итого по расходу\s+998,40/], ['конец', /Остаток на 13\.10\.2026\s+249,60/],
    ['приложение', /Батон\s+40 шт\s+1 248,00\s+32 шт\s+8 шт\s+249,60/], ['документов', /Приложение: 2 документа/]]) {
    expect(re.test(text), `${what}: не найдено в отчёте\n` + text.slice(0, 600));
}
expect(!document.querySelector('.warnings'), 'предупреждение о себестоимости при полной себестоимости');
log.push('отчёт: 1 248,00 - 998,40 = 249,60, приложение по батону');
