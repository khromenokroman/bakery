// Фильтр документов по виду (#2): поступление, реализация, возврат, списание за октябрь. «Реализация» оставляет
// только реализацию, добавление «Возврат» - реализацию и возврат, вид сохраняется при смене дат и в адресе,
// повторный щелчок снимает вид, «Все» сбрасывает фильтр.
const post = async (path, body) => {
    const res = await fetch('/api/' + path, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body || {})});
    const data = await res.json();
    if (!res.ok) throw new Error(path + ': ' + data.error);
    return data;
};
const bread = (await (await fetch('/api/items')).json()).find(i => i.name === 'Батон').id;
const v1 = (await post('price-types', {name: 'Вариант 1'})).id;
await post(`docs/${(await post('docs', {type: 'production', date: '2026-10-11', lines: [{item_id: bread, qty: 40000}]})).id}/post`);
const sale = await post('docs', {type: 'sale', date: '2026-10-12', price_type_id: v1, lines: [{item_id: bread, qty: 30000, price_kop: 4500}]});
await post(`docs/${sale.id}/post`);
await post('docs', {type: 'return', date: '2026-10-13', base_id: sale.id, lines: [{item_id: bread, qty: 1000}]});
await post('docs', {type: 'writeoff', date: '2026-10-13', comment: 'брак', lines: [{item_id: bread, qty: 1000}]});
location.hash = '#/docs?from=2026-10-01&to=2026-10-31&r=1';
await sleep(600);

// виды документов в строках таблицы
const shown = () => [...document.querySelectorAll('tbody tr td:nth-child(2)')].map(td => td.textContent.replace(/ № .*/, '')).sort().join(', ');
const active = () => [...document.querySelectorAll('.tabs button.active')].map(b => b.textContent).join(', ');
const step = async (what, click, want, wantActive) => {
    click();
    await sleep(600);
    expect(shown() === want, `${what}: в списке «${shown()}», ожидалось «${want}»`);
    expect(active() === wantActive, `${what}: выбраны «${active()}», ожидалось «${wantActive}»`);
    log.push(`${what}: ${want}`);
};
const all = 'Возврат от покупателя, Поступление из производства, Реализация, Списание';

expect(shown() === all, `без фильтра в списке «${shown()}», ожидалось «${all}»`);
expect(active() === 'Все', 'без фильтра должна быть выбрана «Все»: ' + active());
await step('только реализация', () => btn('Реализация').click(), 'Реализация', 'Реализация');
await step('реализация и возврат', () => btn('Возврат').click(), 'Возврат от покупателя, Реализация', 'Реализация, Возврат');
expect(location.hash.includes('types=sale,return'), 'виды не попали в адрес: ' + location.hash);

const from = document.querySelector('.toolbar input[type=date]');
await step('с 13.10 вид сохраняется', () => { from.value = '2026-10-13'; from.dispatchEvent(new Event('change')); },
    'Возврат от покупателя', 'Реализация, Возврат');
await step('снять реализацию', () => btn('Реализация').click(), 'Возврат от покупателя', 'Возврат');
await step('инвентаризаций нет', () => { location.hash = '#/docs?from=2026-10-01&to=2026-10-31&types=inventory'; }, '', 'Инвентаризация');
expect(document.querySelector('tbody').textContent.includes('документов выбранных видов нет'), 'нет сообщения о пустом списке выбранных видов');
await step('снять последний вид - все', () => btn('Инвентаризация').click(), all, 'Все');
await step('только списание', () => btn('Списание').click(), 'Списание', 'Списание');
await step('«Все»', () => btn('Все').click(), all, 'Все');

// из документа «← Документы» возвращает к списку с тем же фильтром
await step('только возврат', () => btn('Возврат').click(), 'Возврат от покупателя', 'Возврат');
document.querySelector('tbody tr.link').click();
await sleep(600);
await step('назад из возврата', () => document.querySelector('a.back').click(), 'Возврат от покупателя', 'Возврат');
