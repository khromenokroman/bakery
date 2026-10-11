// Реквизиты и печать: заполнить организацию и включить НДС, поставить батону ставку 10%, распечатать
// реализацию 40 батонов по 45,00: в шапке реквизиты, НДС 163,64, сумма прописью, подпись кладовщика.
const post = async (path, body, method = 'POST') => {
    const res = await fetch('/api/' + path, {method, headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body || {})});
    const data = await res.json();
    if (!res.ok) throw new Error(path + ': ' + data.error);
    return data;
};
const input = label => [...document.querySelectorAll('.fields label')].find(l => l.textContent.startsWith(label)).querySelector('input, select');
location.hash = '#/settings';
await sleep(600);
input('Название').value = 'ООО «Хлебный дом»';
input('ИНН').value = '7707083893';
input('Отпуск произвёл').value = 'Петров А. С.';
document.querySelector('input[type=checkbox]').checked = true;
btn('Сохранить').click();
await sleep(500);
expect(document.getElementById('message').textContent === 'Реквизиты сохранены', 'реквизиты: ' + document.getElementById('message').textContent);
input('ИНН').value = '7707083894';
btn('Сохранить').click();
await sleep(500);
expect(document.getElementById('message').textContent.includes('не сходится контрольное число'), 'неверный ИНН принят');
log.push('неверный ИНН: ' + document.getElementById('message').textContent);

const bread = (await (await fetch('/api/items')).json()).find(i => i.name === 'Батон').id;
location.hash = '#/items/' + bread;
await sleep(600);
input('НДС').value = '10';
btn('Сохранить').click();
await sleep(600);

const v1 = (await post('price-types', {name: 'Опт'})).id;
await post(`docs/${(await post('docs', {type: 'production', date: '2026-10-11', time: '06:00', lines: [{item_id: bread, qty: 40000}]})).id}/post`);
const sale = await post('docs', {type: 'sale', date: '2026-10-12', time: '08:30', price_type_id: v1, lines: [{item_id: bread, qty: 40000, price_kop: 4500}]});
location.hash = '#/print/' + sale.id;
await sleep(700);
const text = document.querySelector('.print-sheet').innerText.replace(/ /g, ' ');
expect(text.includes('ООО «Хлебный дом», ИНН 7707083893'), 'нет реквизитов в шапке');
expect(text.includes('Реализация № 1 от 12.10.2026 08:30'), 'заголовок: ' + text.slice(0, 200));
expect(text.includes('Итого: 1 800,00 ₽, в т.ч. НДС 163,64 ₽'), 'итог с НДС: ' + text);
expect(text.includes('на сумму Одна тысяча восемьсот рублей 00 копеек'), 'сумма прописью');
expect(text.includes('/Петров А. С./'), 'подпись кладовщика');
expect(document.body.classList.contains('print-page') && getComputedStyle(document.querySelector('header')).display === 'none', 'на печатной странице видна шапка сайта');
log.push('печать реализации: реквизиты, НДС 163,64, прописью, подпись');
