// Отчёт о продажах. В seed-базе уже есть: себестоимость батона 31,20 (10.10), заявка на 45 батонов
// (не реализация, в отчёт не входит), цена «Вариант 1» 45,00. Сценарий добавляет: реализацию
// 12.10 - батон 30 по 45,00 (себестоимость 31,20), торт 5 кг по 150,00 (себестоимость 100,00,
// установлена сценарием); возврат 13.10 - 5 батонов.
// Без возврата: батон 30 / 936,00 / 1 350,00 / 414,00 (44 %); торт 5 / 500,00 / 750,00 / 250,00 (50 %);
// итог 35 / 1 436,00 / 2 100,00 / 664,00 (46 %).
// С возвратом (себестоимость возврата на дату возврата 31,20): батон (30-5) × 31,20 = 780,00 /
// (1 350-225) = 1 125,00 / 345,00 (44 %); торт 500,00 / 750,00 / 250,00; итог 30 / 1 280,00 /
// 1 875,00 / 595,00 (46 %). По группе «Кондитерка» только торт.
// Ключевое: #/sales открывает отчёт, а не номенклатуру.
const post = async (path, body) => {
    const res = await fetch('/api/' + path, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body || {})});
    const data = await res.json();
    if (!res.ok) throw new Error(path + ': ' + data.error);
    return data;
};
const items = await (await fetch('/api/items')).json();
const id = name => items.find(i => i.name === name).id;
const bread = id('Батон');
const cake = id('Торт');
const pastry = (await post('groups', {name: 'Кондитерка'})).id;
await fetch(`/api/items/${cake}`, {method: 'PUT', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({group_id: pastry})});
await post(`docs/${(await post('docs', {type: 'prices', date: '2026-10-10', price_type_id: 1, lines: [{item_id: bread, price_kop: 3120}, {item_id: cake, price_kop: 10000}]})).id}/post`);
await post(`docs/${(await post('docs', {type: 'production', date: '2026-10-11', lines: [{item_id: bread, qty: 40000}, {item_id: cake, qty: 6000}]})).id}/post`);
const shop = (await post('partners', {name: 'Колосок'})).id;
const v1 = (await post('price-types', {name: 'Розница'})).id;
const sale = await post('docs', {type: 'sale', date: '2026-10-12', price_type_id: v1, partner_id: shop, lines: [{item_id: bread, qty: 30000, price_kop: 4500}, {item_id: cake, qty: 5000, price_kop: 15000}]});
await post(`docs/${sale.id}/post`);
const saleId = sale.id;

location.hash = '#/sales?from=2026-10-12&to=2026-10-13';
await sleep(900);
expect(document.querySelector('h2') && document.querySelector('h2').textContent.includes('Отчёт о продажах'),
    'заголовок должен быть «Отчёт о продажах», а не «Номенклатура»: ' + (document.querySelector('h2') ? document.querySelector('h2').textContent : 'нет h2'));
expect(!document.body.innerText.includes('Рецептура'), 'на странице продаж не должно быть редактора рецептуры');

const text = document.querySelector('main').innerText.replace(/\u00a0/g, ' ');
expect(text.includes('Батон'), 'нет строки «Батон»');
expect(text.includes('Торт'), 'нет строки «Торт»');
expect(/Батон[\s\S]*30[\s\S]*936,00[\s\S]*1 350,00[\s\S]*414,00[\s\S]*44 ?%/.test(text),
    'строка батона 30 / 936,00 / 1 350,00 / 414,00 / 44%: ' + text.slice(text.indexOf('Батон'), text.indexOf('Батон') + 200));
expect(/Итого:[\s\S]*35[\s\S]*1 436,00[\s\S]*2 100,00[\s\S]*664,00[\s\S]*46 ?%/.test(text),
    'итого 35 / 1 436,00 / 2 100,00 / 664,00 / 46%: ' + text.slice(text.indexOf('Итого')));
log.push('без возврата: батон 30 (936,00/1 350,00/414,00), итог 1 436,00/2 100,00/664,00');

// возврат 5 батонов: себестоимость возврата на дату возврата 31,20 -> нетто батона (30-5)×31,20 = 780,00
await post(`docs/${(await post('docs', {type: 'return', date: '2026-10-13', base_id: saleId, lines: [{item_id: bread, qty: 5000}]})).id}/post`);
location.hash = '#/sales?from=2026-10-12&to=2026-10-13&r=1';
await sleep(900);
const t2 = document.querySelector('main').innerText.replace(/\u00a0/g, ' ');
expect(/Батон[\s\S]*25[\s\S]*780,00[\s\S]*1 125,00[\s\S]*345,00[\s\S]*44 ?%/.test(t2),
    'после возврата батон 25 / 780,00 / 1 125,00 / 345,00 / 44%: ' + t2.slice(t2.indexOf('Батон'), t2.indexOf('Батон') + 220));
expect(/Итого:[\s\S]*30[\s\S]*1 280,00[\s\S]*1 875,00[\s\S]*595,00[\s\S]*46 ?%/.test(t2),
    'после возврата итог 30 / 1 280,00 / 1 875,00 / 595,00 / 46%: ' + t2.slice(t2.indexOf('Итого')));
log.push('с возвратом: батон 25 (780,00/1 125,00/345,00), итог 1 280,00/1 875,00/595,00');

// группа «Кондитерка» - только торт
location.hash = '#/sales?from=2026-10-12&to=2026-10-13&group=' + pastry;
await sleep(900);
const t3 = document.querySelector('main').innerText.replace(/\u00a0/g, ' ');
expect(t3.includes('Торт'), 'в группе «Кондитерка» должен быть торт');
expect(!t3.includes('Батон'), 'в группе «Кондитерка» не должно быть батона');
expect(/Итого:[\s\S]*5[\s\S]*500,00[\s\S]*750,00[\s\S]*250,00[\s\S]*50 ?%/.test(t3),
    'по группе «Кондитерка» итого 5 / 500,00 / 750,00 / 250,00 / 50%: ' + t3.slice(t3.indexOf('Итого')));
log.push('группа «Кондитерка»: только торт 5 / 500,00 / 750,00 / 250,00');
