// Перепроведение после правки задним числом: поступление 10.10 08:00 40 батонов, инвентаризация 20:00
// насчитала 38, реализация 21:00 30 шт. В карточке поступления 40 -> 50 («Сохранить и перепровести»):
// предупреждение с причиной; после инвентаризации стало 48, и проходит реализация ещё 15 шт в 22:00.
// Перепроведение останавливается на инвентаризации (после неё 38 - 30 - 15 < 0) с документом и
// причиной; после перезагрузки страницы место остановки и ошибка видны (#3); после отмены второй
// реализации «Продолжить» доходит до конца без повтора поступления, предупреждение исчезает.
const post = async (path, body) => {
    const res = await fetch('/api/' + path, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body || {})});
    const data = await res.json();
    if (!res.ok) throw new Error(path + ': ' + data.error);
    return data;
};
const items = await (await fetch('/api/items')).json();
const bread = items.find(i => i.name === 'Батон').id;
const buyer = (await post('partners', {name: 'Покупатель'})).id;
const opt = (await post('price-types', {name: 'Опт'})).id;
const prod = (await post('docs', {type: 'production', date: '2026-10-10', time: '08:00', lines: [{item_id: bread, qty: 40000}]})).id;
await post(`docs/${prod}/post`);
await post(`docs/${(await post('docs', {type: 'inventory', date: '2026-10-10', time: '20:00', lines: [{item_id: bread, qty: 38000}]})).id}/post`);
await post(`docs/${(await post('docs', {type: 'sale', partner_id: buyer, date: '2026-10-10', time: '21:00', price_type_id: opt, lines: [{item_id: bread, qty: 30000, price_kop: 4500}]})).id}/post`);
const banner = document.getElementById('repost-banner');
expect(banner.hidden, 'документы проведены по порядку, предупреждения быть не должно');

location.hash = '#/docs/' + prod;
await sleep(800);
const qty = document.querySelector('tbody input.qty');
qty.value = '50';
qty.dispatchEvent(new Event('input'));
btn('Сохранить и перепровести').click();
await sleep(900);
expect(!banner.hidden, 'нет предупреждения после правки задним числом: ' + document.getElementById('message').textContent);
expect(banner.textContent.includes('Поступление из производства № 1 от 10.10.2026 08:00') && banner.textContent.includes('с 10.10.2026 08:00 (документов: 3)'),
    'предупреждение: ' + banner.textContent);
log.push(banner.textContent);

const late = (await post('docs', {type: 'sale', partner_id: buyer, date: '2026-10-10', time: '22:00', price_type_id: opt, lines: [{item_id: bread, qty: 15000, price_kop: 4500}]})).id;
await post(`docs/${late}/post`);

banner.querySelector('a[href="#/repost"]').click();
await sleep(800);
expect(banner.hidden, 'на странице перепроведения предупреждение над страницей не нужно');
expect(document.querySelector('main').textContent.includes('документов к перепроведению: 4'), 'счётчик: ' + document.querySelector('main').textContent);
btn('Перепровести').click();
await sleep(1200);
const error = document.querySelector('.repost-error');
expect(error, 'перепроведение должно остановиться на ошибке: ' + document.querySelector('.repost-progress').textContent);
expect(error.textContent.includes('Инвентаризация № 1 от 10.10.2026 20:00') && error.textContent.includes('уйдёт в минус'), 'ошибка: ' + error.textContent);
expect(error.querySelector('a').getAttribute('href').startsWith('#/docs/'), 'нет ссылки на документ');
expect(document.querySelector('progress').value === 1, 'прогресс: перепроведено ' + document.querySelector('progress').value);
expect(btn('Продолжить'), 'нет кнопки «Продолжить»');
log.push(error.textContent);

// как после перезагрузки или с другого компьютера: страница заново, место остановки - с сервера
location.hash = '#/docs';
await sleep(600);
location.hash = '#/repost';
await sleep(900);
const resumed = document.querySelector('.repost-progress').textContent;
expect(resumed.includes('не закончено: перепроведено 1, осталось 3'), 'после перезагрузки нет незавершённого прогона: ' + resumed);
expect(document.querySelector('.repost-error') && document.querySelector('.repost-error').textContent.includes('Инвентаризация № 1'),
    'после перезагрузки нет ошибки остановки');
expect(btn('Продолжить') && btn('Начать заново'), 'после перезагрузки нужны «Продолжить» и «Начать заново»');
log.push('после перезагрузки: ' + resumed.slice(0, 90));

await post(`docs/${late}/unpost`);
btn('Продолжить').click();
await sleep(1200);
const result = document.querySelector('.repost-progress').textContent;
expect(result.includes('Готово: перепроведено документов 3'), 'итог: ' + result);
await sleep(300);
expect(banner.hidden, 'предупреждение должно исчезнуть');
const stock = (await (await fetch('/api/stock')).json()).map(s => s.name + '=' + s.qty).join(', ');
expect(stock === 'Батон=8000', 'остаток: насчитано 38 - продано 30, а не ' + stock);
log.push(result + '; ' + stock);
