// Реализация: тип цен «Вариант 1» (батон 45,00, торта нет), себестоимость батона 31,20, на складе 50 батонов
// и 5 кг торта. Заполнить по заявкам (45 батонов, 5 кг торта), провести без цены торта - отказ; ввести
// цену торта 700, поправить батон на 44 (ручная цена подсвечена), провести; проверить суммы и остаток.
const post = async (path, body) => {
    const res = await fetch('/api/' + path, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body || {})});
    const data = await res.json();
    if (!res.ok) throw new Error(path + ': ' + data.error);
    return data;
};
const items = await (await fetch('/api/items')).json();
const id = name => items.find(i => i.name === name).id;
const v1 = (await post('price-types', {name: 'Вариант 1'})).id;
await post(`docs/${(await post('docs', {type: 'prices', date: '2026-10-10', price_type_id: v1, lines: [{item_id: id('Батон'), price_kop: 4500}]})).id}/post`);
await post(`docs/${(await post('docs', {type: 'prices', date: '2026-10-10', price_type_id: 1, lines: [{item_id: id('Батон'), price_kop: 3120}]})).id}/post`);
await post(`docs/${(await post('docs', {type: 'production', date: '2026-10-11', lines: [{item_id: id('Батон'), qty: 50000}, {item_id: id('Торт'), qty: 5000}]})).id}/post`);
location.hash = '#/docs/new?type=sale&date=2026-10-12';
await sleep(800);

const field = label => [...document.querySelectorAll('.fields label')].find(l => l.textContent.startsWith(label)).querySelector('select, input');
const typeSelect = field('Тип цен');
typeSelect.value = String(v1);
typeSelect.dispatchEvent(new Event('change'));
await sleep(500);
btn('Заполнить по заявкам').click();
await sleep(500);
btn('Заполнить').click();
await sleep(600);
const rowOf = name => [...document.querySelectorAll('tbody tr')].find(r => r.querySelector('select').selectedOptions[0].text === name);
expect(rowOf('Батон').querySelector('input.money').value === '45,00', 'цена батона не из типа: ' + rowOf('Батон').querySelector('input.money').value);
expect(rowOf('Торт').querySelector('input.money').classList.contains('missing'), 'торт без цены не подсвечен');
const totalsText = () => document.querySelector('.totals').textContent.replace(/ /g, ' ');
expect(totalsText().includes('без цены строк: 1'), 'итоги: ' + totalsText());

btn('Сохранить и провести').click();
await sleep(900);
const message = document.getElementById('message').textContent;
log.push('без цены: ' + message);
expect(message.includes('нет цены в типе «Вариант 1» на 12.10.2026: Торт'), 'нет отказа без цены: ' + message);
expect(/^#\/docs\/\d+$/.test(location.hash) && document.querySelector('.status.draft'), 'документ должен сохраниться непроведённым');

rowOf('Торт').querySelector('input.money').value = '700';
const bread = rowOf('Батон').querySelector('input.money');
bread.value = '44';
bread.dispatchEvent(new Event('input'));
expect(bread.classList.contains('manual'), 'поправленная цена батона не подсвечена');
log.push('ручная цена: ' + bread.title);
expect(totalsText().includes('Сумма: 5 480,00'), 'сумма 45×44 + 5×700: ' + totalsText());
expect(totalsText().includes('Себестоимость: 1 404,00 ₽ *'), 'себестоимость 45×31,20 со звёздочкой: ' + totalsText());
expect(!totalsText().includes('Наценка:') && totalsText().includes('наценка не считается'), 'наценка при неполной себестоимости: ' + totalsText());
log.push('итоги: ' + totalsText());

btn('Сохранить и провести').click();
await sleep(900);
expect(document.querySelector('.status.posted'), 'реализация не проведена: ' + document.getElementById('message').textContent);
const stock = await (await fetch('/api/stock')).json();
expect(stock.map(s => s.name + '=' + s.qty).join(', ') === 'Батон=5000', 'остатки: ' + JSON.stringify(stock));
log.push('остаток: Батон 5 шт');
