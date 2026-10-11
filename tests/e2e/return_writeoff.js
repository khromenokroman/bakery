// Возврат и списание: на складе 40 батонов, «Колоску» продано 30 (проведено). Оформить возврат из реализации
// (предзаполнено 30), вернуть 5, провести; в реализации ссылка на возврат, отменить её нельзя. Списание без
// причины - отказ, с причиной «брак» - проводится.
const post = async (path, body) => {
    const res = await fetch('/api/' + path, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body || {})});
    const data = await res.json();
    if (!res.ok) throw new Error(path + ': ' + data.error);
    return data;
};
const items = await (await fetch('/api/items')).json();
const bread = items.find(i => i.name === 'Батон').id;
const v1 = (await post('price-types', {name: 'Вариант 1'})).id;
const shop = (await post('partners', {name: 'Колосок'})).id;
await post(`docs/${(await post('docs', {type: 'production', date: '2026-10-11', lines: [{item_id: bread, qty: 40000}]})).id}/post`);
const sale = await post('docs', {type: 'sale', date: '2026-10-12', price_type_id: v1, partner_id: shop, lines: [{item_id: bread, qty: 30000, price_kop: 4500}]});
await post(`docs/${sale.id}/post`);
const stockOf = async () => (await (await fetch('/api/stock')).json()).map(s => s.name + '=' + s.qty).join(', ');

location.hash = '#/docs/' + sale.id;
await sleep(700);
btn('Оформить возврат').click();
await sleep(800);
const row = document.querySelector('tbody tr');
expect(document.querySelector('main').textContent.includes('клиент Колосок'), 'нет ссылки на реализацию с клиентом');
expect(row.querySelector('input.qty').value === '30', 'возврат не предзаполнен проданным: ' + row.querySelector('input.qty').value);
expect(row.querySelector('select').disabled && row.querySelector('input.money').readOnly, 'товар и цена в возврате должны быть фиксированы');
row.querySelector('input.qty').value = '5';
btn('Сохранить и провести').click();
await sleep(900);
expect(document.querySelector('.status.posted'), 'возврат не проведён: ' + document.getElementById('message').textContent);
expect(await stockOf() === 'Батон=15000', 'остаток после возврата: ' + await stockOf());
log.push('возврат 5 шт проведён, остаток 15');

location.hash = '#/docs/' + sale.id;
await sleep(800);
const link = [...document.querySelectorAll('main a')].find(a => a.textContent === '№ 1');
expect(link && /#\/docs\/\d+$/.test(link.href), 'в реализации нет ссылки на возврат № 1');
btn('Отменить проведение').click();
await sleep(800);
const msg = document.getElementById('message').textContent;
expect(msg.includes('проведены возвраты № 1'), 'реализацию с возвратом отменили: ' + msg);
log.push('отмена реализации: ' + msg);

location.hash = '#/docs/new?type=writeoff&date=2026-10-13';
await sleep(700);
const w = document.querySelector('tbody tr');
const select = w.querySelector('select');
select.value = String(bread);
select.dispatchEvent(new Event('change'));
w.querySelector('input.qty').value = '2';
btn('Сохранить и провести').click();
await sleep(800);
expect(document.getElementById('message').textContent.includes('укажите причину списания'), 'списание без причины прошло');
[...document.querySelectorAll('.fields label')].find(l => l.textContent.startsWith('Причина')).querySelector('input').value = 'брак';
btn('Сохранить и провести').click();
await sleep(900);
expect(document.querySelector('.status.posted'), 'списание не проведено: ' + document.getElementById('message').textContent);
expect(await stockOf() === 'Батон=13000', 'остаток после списания: ' + await stockOf());
log.push('списание 2 шт с причиной «брак», остаток 13');
