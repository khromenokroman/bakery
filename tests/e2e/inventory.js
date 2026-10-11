// Инвентаризация по всей продукции: на складе 40 батонов и 5 кг торта. Заполнить учётными остатками,
// исправить факт батона на 38 (разница -2), убрать торт (предупреждение об обнулении), провести:
// остаётся только батон 38.
const post = async (path, body) => {
    const res = await fetch('/api/' + path, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body || {})});
    const data = await res.json();
    if (!res.ok) throw new Error(path + ': ' + data.error);
    return data;
};
const items = await (await fetch('/api/items')).json();
const id = name => items.find(i => i.name === name).id;
await post(`docs/${(await post('docs', {type: 'production', date: '2026-10-11', time: '08:00', lines: [{item_id: id('Батон'), qty: 40000}, {item_id: id('Торт'), qty: 5000}]})).id}/post`);

location.hash = '#/docs/new?type=inventory&date=2026-10-12';
await sleep(800);
btn('Заполнить учётными остатками').click();
await sleep(700);
const rowOf = name => [...document.querySelectorAll('tbody tr')].find(r => r.querySelector('select').selectedOptions[0].text === name);
expect(rowOf('Батон') && rowOf('Торт'), 'строки не заполнены учётом');
expect(rowOf('Торт').querySelector('td.book').textContent === '5', 'учёт торта: ' + rowOf('Торт').querySelector('td.book').textContent);

const bread = rowOf('Батон').querySelector('input.qty');
bread.value = '38';
bread.dispatchEvent(new Event('input'));
expect(rowOf('Батон').querySelector('td.diff').textContent === '-2', 'разница батона: ' + rowOf('Батон').querySelector('td.diff').textContent);
rowOf('Торт').querySelector('button.danger').click();
await sleep(100);
const warn = document.querySelector('.warnings:not([hidden])');
expect(warn && warn.textContent.includes('будут обнулены') && warn.textContent.includes('Торт 5 кг'), 'нет предупреждения об обнулении торта');
log.push(warn.textContent);

btn('Сохранить и провести').click();
await sleep(900);
expect(document.querySelector('.status.posted'), 'инвентаризация не проведена: ' + document.getElementById('message').textContent);
const stock = (await (await fetch('/api/stock')).json()).map(s => s.name + '=' + s.qty).join(', ');
expect(stock === 'Батон=38000', 'остатки после инвентаризации: ' + stock);
expect(rowOf('Батон').querySelector('td.book').textContent === '40', 'после проведения учёт должен быть без самой инвентаризации');
log.push('проведено: остался только Батон 38');
