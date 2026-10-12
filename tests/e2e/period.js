// Закрытие периода (#4): батон, поступление 30.09 проведено, списание 30.09 черновиком, инвентаризация 01.10 проведена.
// «Организация» -> закрыть по 30.09: документы 30.09 только для просмотра (нет кнопок изменения, поля выключены,
// печать есть), создать документ на 30.09 нельзя. Открыть все периоды (с подтверждением), исправить поступление
// задним числом - закрыть по 30.09 нельзя, пока документы ждут перепроведения.
const call = async (method, path, body) => {
    const res = await fetch('/api/' + path, {method, headers: {'Content-Type': 'application/json'}, body: body === undefined ? undefined : JSON.stringify(body)});
    const data = await res.json();
    if (!res.ok) throw new Error(path + ': ' + data.error);
    return data;
};
const post = (path, body) => call('POST', path, body || {});
const bread = (await (await fetch('/api/items')).json()).find(i => i.name === 'Батон').id;
const prod = await post('docs', {type: 'production', date: '2026-09-30', time: '08:00', lines: [{item_id: bread, qty: 10000}]});
await post(`docs/${prod.id}/post`);
const draft = await post('docs', {type: 'writeoff', date: '2026-09-30', time: '12:00', comment: 'брак', lines: [{item_id: bread, qty: 1000}]});
await post(`docs/${(await post('docs', {type: 'inventory', date: '2026-10-01', time: '20:00', lines: [{item_id: bread, qty: 9000}]})).id}/post`);

const text = () => document.querySelector('main').textContent;
const message = () => document.getElementById('message').textContent;
const close = async date => {
    const input = [...document.querySelectorAll('.toolbar input[type=date]')].pop();
    input.value = date;
    btn('Закрыть период').click();
    await sleep(700);
};

location.hash = '#/settings?r=1';
await sleep(700);
expect(text().includes('Период не закрыт'), 'в новой базе период должен быть открыт');
await close('2026-09-30');
expect(message().includes('Период закрыт по 30.09.2026'), 'нет сообщения о закрытии: ' + message());
expect(text().includes('Закрыт по 30.09.2026'), 'на странице нет даты закрытия');
log.push('закрыто по 30.09.2026');

location.hash = '#/docs/' + prod.id;
await sleep(700);
for (const b of ['Сохранить и перепровести', 'Отменить проведение', 'Заполнить по заявкам', '+ строка']) {
    expect(!btn(b), `в закрытом проведённом документе есть кнопка «${b}»`);
}
expect(btn('Печать'), 'печать закрытого документа должна остаться');
expect(document.querySelector('.warnings') && document.querySelector('.warnings').textContent.includes('Период по 30.09.2026 закрыт'), 'нет предупреждения о закрытом периоде');
const enabled = [...document.querySelectorAll('.fields input, tbody input, tbody select')].filter(e => !e.disabled);
expect(enabled.length === 0, 'поля закрытого документа должны быть выключены, включено: ' + enabled.length);
log.push('поступление 30.09: только просмотр и печать');

location.hash = '#/docs/' + draft.id;
await sleep(700);
for (const b of ['Сохранить', 'Сохранить и провести', 'Удалить']) {
    expect(!btn(b), `в закрытом черновике есть кнопка «${b}»`);
}
let refused = '';
try { await post('docs', {type: 'writeoff', date: '2026-09-30', comment: 'бой', lines: [{item_id: bread, qty: 1000}]}); } catch (e) { refused = e.message; }
expect(refused.includes('период по 30.09.2026 закрыт'), 'документ на 30.09 должен отклоняться: ' + refused);
log.push('черновик 30.09 не провести и не удалить, новый на 30.09 не создать');

location.hash = '#/settings?r=2';
await sleep(700);
btn('Открыть все периоды').click();
await sleep(700);
expect(text().includes('Период не закрыт'), 'период не открылся');
await call('PUT', `docs/${prod.id}`, {date: '2026-09-30', time: '08:00', lines: [{item_id: bread, qty: 12000}]});
await sleep(300);
await close('2026-09-30');
expect(message().includes('ждут перепроведения'), 'закрытие при незавершённом перепроведении должно отклоняться: ' + message());
expect(text().includes('Период не закрыт'), 'после отказа период не должен закрыться');
log.push('исправлено задним числом: «' + message() + '»');
